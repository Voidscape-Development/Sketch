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
	static Remote &get();
	bool enabled() const { return server.isListening(); }
	void setEnabled(bool on);
	QString url() const;
	void regenToken(); // new link; disconnects current clients
	void shutdown();

private:
	Remote();
	struct Client {
		QByteArray buf;
		bool ws = false, ready = false; // ready: browser wants the next frame
		int id;
		BrushState b;
	};
	void read(QTcpSocket *s);
	void http(QTcpSocket *s, Client &c, const QByteArray &req);
	void message(Client &c, const QByteArray &msg);
	static void send(QTcpSocket *s, int opcode, const QByteArray &payload);
	static void rendered(void *param);
	void broadcast(const QByteArray &jpg, uint32_t w, uint32_t h);

	QTcpServer server;
	std::map<QTcpSocket *, Client> clients;
	QString token;
	int port = 4477, nextId = 1;
	std::atomic<bool> want{false};
	gs_texrender_t *texrender = nullptr;
	gs_stagesurf_t *stage = nullptr;
	bool staged = false; // graphics thread only
	uint32_t stagedW = 0, stagedH = 0;
};
