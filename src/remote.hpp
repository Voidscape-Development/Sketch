#pragma once
#include "common.hpp"
#include <QObject>
#include <QTcpServer>
#include <atomic>
#include <map>

class QTcpSocket;

// Serves the remote drawing page over HTTP and receives strokes over a WebSocket.
class Remote : public QObject {
	Q_OBJECT
public:
	struct Guest {
		int id;
		QString name;
		bool allowed, view;
	};
	static Remote &get();
	bool enabled() const { return server.isListening(); }
	void setEnabled(bool on);
	QString url(bool view = false) const;
	void regenToken(); // new links; disconnects current clients
	void shutdown();
	std::vector<Guest> guests() const;
	void allow(int id, bool on);
	void kick(int id);
	bool approve() const { return approveNew; }
	bool locked() const { return lock; }
	void setPolicy(bool approve, bool locked); // approve: new guests wait for the host; locked: only the host draws

signals:
	void guestsChanged();

private:
	Remote();
	struct Client {
		QByteArray buf;
		bool ws = false, ready = false; // ready: browser wants the next frame
		bool view = false, allowed = true;
		int id;
		float zoom = 1;
		QString name;
		BrushState b;
	};
	bool canDraw(const Client &c) const { return !c.view && c.allowed && !lock; }
	void read(QTcpSocket *s);
	void http(QTcpSocket *s, Client &c, const QByteArray &req);
	void message(QTcpSocket *s, Client &c, const QByteArray &msg);
	void state(QTcpSocket *s, const Client &c);
	void rezoom();
	static void send(QTcpSocket *s, int opcode, const QByteArray &payload);
	static void rendered(void *param);
	void broadcast(const QByteArray &jpg, uint32_t w, uint32_t h);

	QTcpServer server;
	std::map<QTcpSocket *, Client> clients;
	QString token, viewToken;
	int port = 4477, nextId = 1;
	bool approveNew = false, lock = false;
	std::atomic<bool> want{false};
	std::atomic<float> zoom{1}; // largest zoom among clients: snapshot resolution
	gs_texrender_t *texrender = nullptr;
	gs_stagesurf_t *stage = nullptr;
	bool staged = false; // graphics thread only
	uint32_t stagedW = 0, stagedH = 0;
};
