#include <QCoreApplication>
#include <QtWidgets/QApplication>
#include <QtGui/QClipboard>
#include <QDebug>
#include <QTcpServer>
#include <QTcpSocket>
#include <QHostAddress>
#include <QByteArray>
#include <QBuffer>
#include <QImage>
#include <list>
#include <mutex>
#include <map>
#include <memory>
#include <chrono>

enum DataType {
    QUERY, TEXT, IMAGE
};

enum ReadStage {
    HEADER, BODY
};

class Worker {
public:
    Worker(QTcpSocket* socket): socket_(socket) {

    }

    void writeImage(QImage img) {
        qDebug() << "writeImage";
        uint64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::system_clock::now().time_since_epoch()).count();
        if (now - setClipboardTime < 1000) {
            qDebug() << "just set clipboard, ignore";
            return;
        }
        QByteArray array;
        QBuffer buf(&array);
        if (!img.save(&buf, "png")){
            qDebug() << "save image failed";
            return;
        }

        writeHeader(DataType::IMAGE, array.size());
        socket_->write(array);
    }

    void writeText(QString str) {
        qDebug() << "writeText";
        uint64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch()).count();
        if (now - setClipboardTime < 1000) {
            qDebug() << "just set clipboard, ignore";
            return;
        }
        QByteArray array = str.toUtf8();
        writeHeader(DataType::TEXT, array.size());
        socket_->write(array);
    }

    void readData() {
        qDebug() << "readData";
        while (socket_->bytesAvailable() > 0) {
            qDebug() << "needRead " << leftToRecv;
            QByteArray data = socket_->read(leftToRecv);
            if (data.isEmpty()) {
                qDebug() << "socket " << socket_ << " read no data";
                return;
            }

            recvBuf.append(data);
            leftToRecv -= data.size();
            if (leftToRecv == 0) {
                if (stage == ReadStage::HEADER) {
                    type = DataType(recvBuf.at(0));
                    leftToRecv = ((recvBuf[1] & 0xFF) << 24) | ((recvBuf[2] & 0xFF) << 16) | ((recvBuf[3] & 0xFF) << 8) | ((recvBuf[4] & 0xFF));
                    if (leftToRecv > 0) {
                        stage = ReadStage::BODY;
                    } else {
                        processData();
                    }
                    recvBuf.clear();
                } else {
                    processData();
                    stage = ReadStage::HEADER;
                    leftToRecv = headerLen;
                    recvBuf.clear();
                }
            }
        }
    }

private:
    void processData() {
        qDebug() << "processData type: " << type << ", len: " << recvBuf.size();
        switch(type) {
        case DataType::IMAGE:{
            QImage img;
            img.loadFromData(recvBuf, "png");
            QClipboard* c = QApplication::clipboard();
            setClipboardTime = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::system_clock::now().time_since_epoch()).count();
            c->setImage(img);
            break;
        }
        case DataType::TEXT:{
            QClipboard* c = QApplication::clipboard();
            QString str = QString::fromUtf8(recvBuf);
            setClipboardTime = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::system_clock::now().time_since_epoch()).count();
            c->setText(str);
            break;
        }
        case DataType::QUERY:
            break;
        default:
            qDebug() << "unknown type: " << type;
            break;
        }
    }

    void writeHeader(DataType type, int size) {
        char header[headerLen];
        header[0] = (char)type;
        header[1] = (size >> 24) & 0xFF;
        header[2] = (size >> 16) & 0xFF;
        header[3] = (size >> 8) & 0xFF;
        header[4] = size & 0xFF;
        socket_->write(header, headerLen);
    }

private:
    const int headerLen = 5;
    QTcpSocket* socket_;
    std::list<QByteArray> sendList;
    QByteArray recvBuf;
    DataType type;
    int leftToRecv = headerLen;
    ReadStage stage = ReadStage::HEADER;
    std::mutex lock;
    uint64_t setClipboardTime = 0;
};

class ClipBoardListener: public QObject {
public:
    ClipBoardListener(bool server, QString ip, int port): isServer(server), ip_(ip), port_(port)    {
        qDebug() << "construct listener";
        b = QApplication::clipboard();
        connect(b, &QClipboard::dataChanged, this, &ClipBoardListener::onClipboardChange, Qt::QueuedConnection);
        if (server) {
            initServer();
        } else {
            connectToServer();
        }
    }
public slots:
    void onClipboardChange() {
        QImage img = b->image();
        QString txt = b->text();
        qDebug() << "clipboard change, text: " << txt << ", image: " << img;
        qDebug() << "image is Null: " << img.isNull();
        if (!img.isNull()) {
            qDebug() << "iterate workerMap";
            for(auto iter = workerMap.begin(); iter != workerMap.end(); iter++) {
                iter->second->writeImage(img);
            }
        }
        if (!txt.isEmpty()) {
            for(auto iter = workerMap.begin(); iter != workerMap.end(); iter++) {
                iter->second->writeText(txt);
            }
        }
    }

private:
    void initServer() {
        int port = 56789;
        server.reset(new QTcpServer(this));
        if (!server->listen(QHostAddress::Any, port)) {
            qDebug() << "listen port " << port << " failed.";
            return;
        } else {
            connect(server.get(), &QTcpServer::newConnection, this, &ClipBoardListener::handleConnection);
        }
        qDebug() << "server start success";
    }

    void connectToServer() {
        qDebug() << "connecting to " << ip_ << ":" << port_;
        client.reset(new QTcpSocket());
        client->connectToHost(QHostAddress(ip_), port_);
        connect(client.get(), &QTcpSocket::connected, [this]{
            qDebug() << "connect to server success";
            workerMap[client.get()] = std::make_unique<Worker>(client.get());
        });
        connect(client.get(), &QTcpSocket::disconnected, [this]{
            qDebug() << "disconnected from server";
            workerMap.erase(client.get());
        });
        connect(client.get(), &QTcpSocket::readyRead, [this]{
            if (workerMap.find(client.get()) == workerMap.end()) {
                qDebug() << "socket not found in workerMap";
                return;
            }

            Worker* w = workerMap[client.get()].get();
            w->readData();
        });
    }

    void handleConnection() {
        qDebug() << "get new connection";
        QTcpSocket* socket = server->nextPendingConnection();
        if (socket) {
            workerMap[socket] = std::make_unique<Worker>(socket);
            connect(socket, &QTcpSocket::readyRead, [this, socket]{
                if (workerMap.find(socket) == workerMap.end()) {
                    qDebug() << "socket not found in workerMap";
                    return;
                }

                Worker* w = workerMap[socket].get();
                w->readData();

            });

            connect(socket, &QTcpSocket::disconnected, [this, socket]{
                qDebug() << "socket " << socket << " disconnected";
                workerMap.erase(socket);
                socket->deleteLater();
            });

            connect(socket, &QAbstractSocket::errorOccurred, [this, socket]{
                qDebug() << "socket " << socket << "error: " << socket->errorString();
                workerMap.erase(socket);
                socket->deleteLater();
            });
        }
    }
private:
    QClipboard* b = nullptr;
    std::unique_ptr<QTcpServer> server = nullptr;
    std::unique_ptr<QTcpSocket> client = nullptr;
    bool isServer = false;
    QString ip_;
    int port_;
    std::map<QTcpSocket*, std::unique_ptr<Worker>> workerMap;

};

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    if (argc < 2) {
        qDebug() << "usage: SyncClipboard <-c/s> [host] [port]";
        return -1;
    }
    bool isServer = false;
    QString ip;
    int port = 0;
    if (argv[1][1] == 's'){
        isServer = true;
    }else {
        ip = QString::fromLatin1(argv[2], strlen(argv[2]));
        port = atoi(argv[3]);
    }
    ClipBoardListener listener(isServer, ip, port);
    return a.exec();
}
