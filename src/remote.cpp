#include "remote.hpp"
#include "canvas.hpp"
#include <QBuffer>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkInterface>
#include <QRandomGenerator>
#include <QThreadPool>
#include <QTcpSocket>
#include <QUrl>
#include <QUrlQuery>

Remote &Remote::get()
{
	static Remote *r = new Remote; // intentionally leaked: must outlive QApplication teardown
	return *r;
}

Remote::Remote()
{
	obs_data_t *cfg = config();
	token = obs_data_get_string(cfg, "remote_token");
	if (token.isEmpty()) {
		token = QString::number(QRandomGenerator::system()->generate64(), 36);
		obs_data_set_string(cfg, "remote_token", token.toUtf8().constData());
	}
	if (obs_data_has_user_value(cfg, "remote_port"))
		port = int(obs_data_get_int(cfg, "remote_port"));
	connect(&server, &QTcpServer::newConnection, this, [this] {
		while (QTcpSocket *s = server.nextPendingConnection()) {
			clients[s].id = nextId++;
			connect(s, &QTcpSocket::readyRead, this, [this, s] { read(s); });
			connect(
				s, &QTcpSocket::disconnected, this,
				[this, s] {
					auto it = clients.find(s);
					if (it == clients.end())
						return;
					if (Canvas *c = canvasOf(getTarget()))
						c->up(it->second.id);
					clients.erase(it);
					s->deleteLater();
				},
				Qt::QueuedConnection);
		}
	});
	if (obs_data_get_bool(cfg, "remote_on"))
		setEnabled(true);
}

void Remote::setEnabled(bool on)
{
	obs_data_set_bool(config(), "remote_on", on);
	if (on == enabled())
		return;
	if (on) {
		if (!server.listen(QHostAddress::Any, quint16(port))) {
			blog(LOG_WARNING, "[sketch] remote drawing: cannot listen on port %d", port);
			return;
		}
		obs_add_main_rendered_callback(rendered, this);
	} else
		shutdown();
}

void Remote::shutdown()
{
	if (server.isListening())
		obs_remove_main_rendered_callback(rendered, this);
	server.close();
	for (auto &[s, c] : clients)
		s->abort();
	obs_enter_graphics();
	gs_texrender_destroy(texrender);
	gs_stagesurface_destroy(stage);
	texrender = nullptr;
	stage = nullptr;
	staged = false;
	obs_leave_graphics();
}

QString Remote::url() const
{
	QString host = "localhost";
	for (const QHostAddress &a : QNetworkInterface::allAddresses())
		if (a.protocol() == QAbstractSocket::IPv4Protocol && !a.isLoopback() && !a.isLinkLocal()) {
			host = a.toString();
			const quint32 ip = a.toIPv4Address();
			if (ip >> 24 == 10 || ip >> 20 == 0xac1 || ip >> 16 == 0xc0a8) // private LAN ranges
				break;
		}
	return QString("http://%1:%2/?t=%3").arg(host).arg(port).arg(token);
}

void Remote::read(QTcpSocket *s)
{
	auto it = clients.find(s);
	if (it == clients.end())
		return;
	Client &c = it->second;
	c.buf += s->readAll();
	if (!c.ws) {
		const qsizetype end = c.buf.indexOf("\r\n\r\n");
		if (end < 0) {
			if (c.buf.size() > 16384)
				s->abort();
			return;
		}
		const QByteArray req = c.buf.left(end);
		c.buf.remove(0, end + 4);
		http(s, c, req);
		if (!c.ws)
			return;
	}
	while (c.buf.size() >= 2) { // WebSocket frames (client frames are always masked)
		const auto *d = reinterpret_cast<const uchar *>(c.buf.constData());
		const int op = d[0] & 0x0f;
		quint64 len = d[1] & 0x7f;
		qsizetype pos = 2;
		if (len == 126) {
			if (c.buf.size() < 4)
				return;
			len = quint64(d[2]) << 8 | d[3];
			pos = 4;
		} else if (len == 127) {
			if (c.buf.size() < 10)
				return;
			len = 0;
			for (int i = 2; i < 10; i++)
				len = len << 8 | d[i];
			pos = 10;
		}
		if (!(d[1] & 0x80) || len > (1u << 20))
			return s->abort();
		if (quint64(c.buf.size()) < pos + 4 + len)
			return;
		QByteArray payload = c.buf.mid(pos + 4, qsizetype(len));
		for (qsizetype i = 0; i < payload.size(); i++)
			payload[i] = char(payload[i] ^ d[pos + i % 4]);
		c.buf.remove(0, pos + 4 + qsizetype(len));
		if (op == 8)
			return s->abort();
		if (op == 9)
			send(s, 10, payload);
		else if (op == 1)
			message(c, payload);
	}
}

void Remote::http(QTcpSocket *s, Client &c, const QByteArray &req)
{
	const QList<QByteArray> lines = req.split('\n');
	const QUrl url(QString::fromUtf8(lines.value(0).split(' ').value(1)));
	const bool authorized = QUrlQuery(url).queryItemValue("t") == token;
	QByteArray key;
	for (const QByteArray &l : lines)
		if (l.toLower().startsWith("sec-websocket-key:"))
			key = l.mid(18).trimmed();
	if (authorized && url.path() == "/ws" && !key.isEmpty()) {
		const QByteArray accept =
			QCryptographicHash::hash(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11", QCryptographicHash::Sha1)
				.toBase64();
		s->write("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
			 "Sec-WebSocket-Accept: " +
			 accept + "\r\n\r\n");
		c.ws = true;
		c.b = brush();
		c.b.image = {};
		c.ready = want = true;
		return;
	}
	QByteArray body = "Sketch: invalid or missing access token", status = "403 Forbidden", type = "text/plain";
	if (authorized && url.path() == "/") {
		char *path = obs_module_file("remote.html");
		QFile f(QString::fromUtf8(path));
		bfree(path);
		if (f.open(QIODevice::ReadOnly)) {
			body = f.readAll();
			status = "200 OK";
			type = "text/html; charset=utf-8";
		}
	}
	s->write("HTTP/1.1 " + status + "\r\nContent-Type: " + type + "\r\nContent-Length: " +
		 QByteArray::number(body.size()) + "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n" + body);
	s->disconnectFromHost();
}

void Remote::message(Client &c, const QByteArray &msg)
{
	const QJsonObject o = QJsonDocument::fromJson(msg).object();
	const QString t = o["t"].toString();
	if (t == "ack") // browser showed the last frame: send the next one
		c.ready = want = true;
	else if (t == "tool")
		c.b.tool = qBound(0, o["v"].toInt(), int(Laser));
	else if (t == "color")
		c.b.color = QColor(o["v"].toString());
	else if (t == "size")
		c.b.size = float(qBound(1.0, o["v"].toDouble(), 64.0));
	Canvas *cv = canvasOf(getTarget());
	if (!cv || !cv->w)
		return;
	const QPointF p(o["x"].toDouble() * cv->w, o["y"].toDouble() * cv->h);
	const float pr = float(o["p"].toDouble(1));
	if (t == "down")
		cv->down(c.id, p, pr, c.b, o["text"].toString().left(500));
	else if (t == "move")
		cv->move(c.id, p, pr);
	else if (t == "up")
		cv->up(c.id);
	else if (t == "undo")
		cv->undo();
	else if (t == "redo")
		cv->redo();
	else if (t == "clear")
		cv->clear();
}

void Remote::send(QTcpSocket *s, int opcode, const QByteArray &payload)
{
	QByteArray f(1, char(0x80 | opcode));
	const qsizetype n = payload.size();
	if (n < 126)
		f += char(n);
	else if (n < 65536)
		f += char(126), f += char(n >> 8), f += char(n & 0xff);
	else {
		f += char(127);
		for (int i = 7; i >= 0; i--)
			f += char((quint64(n) >> (i * 8)) & 0xff);
	}
	s->write(f + payload);
}

void Remote::broadcast(const QByteArray &jpg, uint32_t w, uint32_t h)
{
	const QByteArray size = QString(R"({"t":"size","w":%1,"h":%2})").arg(w).arg(h).toUtf8();
	for (auto &[s, c] : clients)
		if (c.ws && c.ready) {
			c.ready = false;
			send(s, 1, size);
			send(s, 2, jpg);
		}
}

void Remote::rendered(void *param)
{
	auto r = static_cast<Remote *>(param);
	uint8_t *data;
	uint32_t linesize;
	if (r->staged && gs_stagesurface_map(r->stage, &data, &linesize)) { // read last frame's copy: no GPU stall
		r->staged = false;
		const uint32_t tw = gs_stagesurface_get_width(r->stage), th = gs_stagesurface_get_height(r->stage);
		QImage img = QImage(data, int(tw), int(th), int(linesize), QImage::Format_RGB32).copy();
		gs_stagesurface_unmap(r->stage);
		QThreadPool::globalInstance()->start([r, img = std::move(img), w = r->stagedW, h = r->stagedH] {
			QByteArray jpg;
			QBuffer buf(&jpg);
			img.save(&buf, "JPG", 70);
			QMetaObject::invokeMethod(r, [r, jpg, w, h] { r->broadcast(jpg, w, h); }, Qt::QueuedConnection);
		});
	}
	if (r->staged || !r->want.exchange(false))
		return;
	OBSSource owner = getTarget();
	obs_source_t *rs = owner ? renderSourceOf(owner) : nullptr;
	const uint32_t sw = rs ? obs_source_get_width(rs) : 0, sh = rs ? obs_source_get_height(rs) : 0;
	if (!sw || !sh)
		return void(r->want = true); // retry once the target has a size
	const float s = std::min(1.f, 960.f / float(std::max(sw, sh)));
	const uint32_t tw = std::max(1u, uint32_t(sw * s)), th = std::max(1u, uint32_t(sh * s));
	if (!r->texrender)
		r->texrender = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
	gs_texrender_reset(r->texrender);
	if (!gs_texrender_begin(r->texrender, tw, th))
		return;
	vec4 bg;
	vec4_set(&bg, .1f, .1f, .12f, 1);
	gs_clear(GS_CLEAR_COLOR, &bg, 0, 0);
	gs_ortho(0, float(sw), 0, float(sh), -100, 100);
	obs_source_video_render(rs);
	gs_texrender_end(r->texrender);
	if (r->stage && (gs_stagesurface_get_width(r->stage) != tw || gs_stagesurface_get_height(r->stage) != th)) {
		gs_stagesurface_destroy(r->stage);
		r->stage = nullptr;
	}
	if (!r->stage)
		r->stage = gs_stagesurface_create(tw, th, GS_BGRA);
	gs_stage_texture(r->stage, gs_texrender_get_texture(r->texrender));
	r->staged = true;
	r->stagedW = sw;
	r->stagedH = sh;
}
