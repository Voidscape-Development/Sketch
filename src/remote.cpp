#include "remote.hpp"
#include "canvas.hpp"
#include <QBuffer>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkInterface>
#include <QRandomGenerator>
#include <QThreadPool>
#include <QTcpSocket>
#include <QUrl>
#include <QUrlQuery>
#include <algorithm>

static const QRgb tagColors[] = {0x8ea6ff, 0xffb86b, 0x7cf2c8, 0xff7ab6, 0xffe55c, 0xff5a5f, 0xb48eff};
static const char *icon =
	R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24"><rect width="24" height="24" rx="5" )"
	R"(fill="#8ea6ff"/><path d="M6.4 17.6l2.8-.7 7.7-7.7-2.1-2.1-7.7 7.7z" fill="none" stroke="#0c0d0f" )"
	R"(stroke-width="1.3" stroke-linejoin="round"/></svg>)";

static QByteArray json(const QJsonObject &o)
{
	return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

static QByteArray brushJson(const BrushState &b)
{
	return json({{"t", "brush"},
		     {"tool", b.tool},
		     {"color", b.color.name()},
		     {"size", b.size},
		     {"opacity", b.opacity},
		     {"stamp", b.stamp},
		     {"style", b.style},
		     {"fill", b.fill},
		     {"ruler", b.ruler}});
}

Remote &Remote::get()
{
	static Remote *r = new Remote; // intentionally leaked: must outlive QApplication teardown
	return *r;
}

Remote::Remote()
{
	obs_data_t *cfg = config();
	token = obs_data_get_string(cfg, "remote_token");
	viewToken = obs_data_get_string(cfg, "remote_view_token");
	if (token.isEmpty() || viewToken.isEmpty())
		regenToken();
	approveNew = obs_data_get_bool(cfg, "remote_approve");
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
						c->cancel(it->second.id, true);
					const bool ws = it->second.ws;
					clients.erase(it);
					s->deleteLater();
					if (ws) {
						rezoom();
						emit guestsChanged();
					}
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

void Remote::regenToken()
{
	token = QString::number(QRandomGenerator::system()->generate64(), 36);
	viewToken = QString::number(QRandomGenerator::system()->generate64(), 36);
	obs_data_set_string(config(), "remote_token", token.toUtf8().constData());
	obs_data_set_string(config(), "remote_view_token", viewToken.toUtf8().constData());
	for (auto &[s, c] : clients) // kick everyone holding the old link
		s->abort();
}

std::vector<Remote::Guest> Remote::guests() const
{
	std::vector<Guest> r;
	for (auto &[s, c] : clients)
		if (c.ws)
			r.push_back({c.id, c.name, c.allowed, c.view});
	std::sort(r.begin(), r.end(), [](auto &a, auto &b) { return a.id < b.id; });
	return r;
}

void Remote::allow(int id, bool on)
{
	for (auto &[s, c] : clients)
		if (c.id == id) {
			c.allowed = on;
			state(s, c);
		}
	if (Canvas *cv = canvasOf(getTarget()); cv && !on)
		cv->cancel(id);
	emit guestsChanged();
}

void Remote::kick(int id)
{
	for (auto &[s, c] : clients)
		if (c.id == id) {
			send(s, 1, json({{"t", "kicked"}})); // the page stops reconnecting
			s->disconnectFromHost();
		}
}

void Remote::setPolicy(bool approve, bool locked)
{
	approveNew = approve;
	obs_data_set_bool(config(), "remote_approve", approve);
	if (lock == locked)
		return;
	lock = locked;
	Canvas *cv = canvasOf(getTarget());
	for (auto &[s, c] : clients)
		if (c.ws) {
			state(s, c);
			if (cv && locked)
				cv->cancel(c.id);
		}
}

void Remote::rezoom()
{
	float z = 1;
	for (auto &[s, c] : clients)
		z = std::max(z, c.zoom);
	zoom = z;
}

void Remote::state(QTcpSocket *s, const Client &c)
{
	send(s, 1, json({{"t", "state"}, {"draw", canDraw(c)}, {"view", c.view}, {"wait", !c.view && !c.allowed}}));
}

QString Remote::url(bool view) const
{
	QString host = "localhost";
	for (const QHostAddress &a : QNetworkInterface::allAddresses())
		if (a.protocol() == QAbstractSocket::IPv4Protocol && !a.isLoopback() && !a.isLinkLocal()) {
			host = a.toString();
			const quint32 ip = a.toIPv4Address();
			if (ip >> 24 == 10 || ip >> 20 == 0xac1 || ip >> 16 == 0xc0a8) // private LAN ranges
				break;
		}
	return QString("http://%1:%2/?t=%3").arg(host).arg(port).arg(view ? viewToken : token);
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
			message(s, c, payload);
	}
}

void Remote::http(QTcpSocket *s, Client &c, const QByteArray &req)
{
	const QList<QByteArray> lines = req.split('\n');
	const QUrl url(QString::fromUtf8(lines.value(0).split(' ').value(1)));
	const QString t = QUrlQuery(url).queryItemValue("t");
	const bool authorized = !t.isEmpty() && (t == token || t == viewToken);
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
		c.view = t == viewToken;
		c.allowed = !approveNew;
		c.name = QString("Guest %1").arg(c.id);
		c.ready = want = true;
		QJsonArray tl, st, pr;
		for (int i = 0; i < ToolCount; i++)
			if (i != Image)
				tl.append(QJsonArray{i, Str(("Tool." + std::string(tools[i].id)).c_str()),
						     tools[i].icon});
		for (int i = 0; i < stampCount; i++)
			st.append(QString::fromUtf8(stamps[i]));
		for (auto &[name, b] : presets())
			pr.append(QJsonObject{{"name", name}, {"color", b.color.name()}});
		send(s, 1, json({{"t", "init"}, {"tools", tl}, {"stamps", st}, {"presets", pr}}));
		state(s, c);
		emit guestsChanged();
		return;
	}
	QByteArray body = "Sketch: invalid or missing access token", status = "403 Forbidden", type = "text/plain";
	if (authorized && url.path() == "/") {
		char *path = obs_module_file("remote.html");
		QFile f(QString::fromUtf8(path));
		bfree(path);
		if (f.open(QIODevice::ReadOnly)) {
			body = f.readAll().replace("{{q}}", "?t=" + t.toUtf8());
			status = "200 OK";
			type = "text/html; charset=utf-8";
		}
	} else if (authorized && url.path() == "/manifest.json") { // lets phones add the page to the home screen
		body = json(
			{{"name", "Sketch Remote"},
			 {"short_name", "Sketch"},
			 {"start_url", "/?t=" + t},
			 {"display", "standalone"},
			 {"background_color", "#0c0d0f"},
			 {"theme_color", "#0c0d0f"},
			 {"icons",
			  QJsonArray{QJsonObject{{"src", "/icon.svg"}, {"sizes", "any"}, {"type", "image/svg+xml"}}}}});
		status = "200 OK";
		type = "application/manifest+json";
	} else if (url.path() == "/icon.svg") {
		body = icon;
		status = "200 OK";
		type = "image/svg+xml";
	}
	s->write("HTTP/1.1 " + status + "\r\nContent-Type: " + type + "\r\nContent-Length: " +
		 QByteArray::number(body.size()) + "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n" + body);
	s->disconnectFromHost();
}

void Remote::message(QTcpSocket *s, Client &c, const QByteArray &msg)
{
	const QJsonObject o = QJsonDocument::fromJson(msg).object();
	const QString t = o["t"].toString();
	const QJsonValue v = o["v"];
	if (t == "ack") // browser showed the last frame: send the next one
		c.ready = want = true;
	else if (t == "hello" && !v.toString().trimmed().isEmpty()) {
		c.name = v.toString().trimmed().left(24);
		emit guestsChanged();
	} else if (t == "zoom") { // sharper snapshots while someone is zoomed in
		c.zoom = float(qBound(1.0, v.toDouble(), 2.5));
		rezoom();
	} else if (t == "tool")
		c.b.tool = v.toInt() == Image ? Pen : qBound(0, v.toInt(), ToolCount - 1);
	else if (t == "color")
		c.b.color = QColor(v.toString());
	else if (t == "size")
		c.b.size = float(qBound(1.0, v.toDouble(), 64.0));
	else if (t == "opacity")
		c.b.opacity = float(qBound(.05, v.toDouble(), 1.0));
	else if (t == "stamp")
		c.b.stamp = qBound(0, v.toInt(), stampCount - 1);
	else if (t == "style")
		c.b.style = qBound(0, v.toInt(), int(Glow));
	else if (t == "fill")
		c.b.fill = v.toBool();
	else if (t == "ruler")
		c.b.ruler = v.toBool();
	else if (t == "preset" && v.toInt() >= 0 && v.toInt() < int(presets().size())) {
		c.b = presets()[size_t(v.toInt())].second;
		c.b.image = {};
		if (c.b.tool == Image)
			c.b.tool = Pen;
		send(s, 1, brushJson(c.b));
	}
	Canvas *cv = canvasOf(getTarget());
	if (!cv || !cv->w || (!canDraw(c) && t != "up"))
		return;
	const QPointF p(o["x"].toDouble() * cv->w, o["y"].toDouble() * cv->h);
	const float pr = float(o["p"].toDouble(1));
	if (t == "down" || t == "move")
		cv->tag(c.id, p, c.name, QColor::fromRgb(tagColors[c.id % std::size(tagColors)]));
	if (t == "down")
		cv->down(c.id, p, pr, c.b, o["text"].toString().left(500));
	else if (t == "move")
		cv->move(c.id, p, pr);
	else if (t == "up")
		cv->up(c.id);
	else if (t == "cancel")
		cv->cancel(c.id);
	else if (t == "undo")
		cv->undo(c.id);
	else if (t == "redo")
		cv->redo(c.id);
	else if (t == "clear")
		cv->clear(c.id);
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
	const float s = std::min(1.f, 960.f * r->zoom / float(std::max(sw, sh)));
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
