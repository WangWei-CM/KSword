#include <QtCore/QCoreApplication>
#include <QtCore/QFile>
#include <QtCore/QList>
#include <QtNetwork/QSslCertificate>
#include <QtNetwork/QSslKey>
#include <cstdio>
int main(int argc, char** argv){ QCoreApplication app(argc, argv); if (argc < 3) { std::printf("usage\n"); return 2; } QFile certFile(QString::fromLocal8Bit(argv[1])); QFile keyFile(QString::fromLocal8Bit(argv[2])); if (!certFile.open(QIODevice::ReadOnly) || !keyFile.open(QIODevice::ReadOnly)) { std::printf("open_failed\n"); return 3; } QByteArray certBytes = certFile.readAll(); QByteArray keyBytes = keyFile.readAll(); QSslCertificate cert(certBytes, QSsl::Pem); QSslKey key1(keyBytes, QSsl::Rsa, QSsl::Pem, QSsl::PrivateKey); QSslKey key2(keyBytes, QSsl::Opaque, QSsl::Pem, QSsl::PrivateKey); std::printf("cert_null=%d\n", cert.isNull() ? 1 : 0); std::printf("key_rsa_null=%d\n", key1.isNull() ? 1 : 0); std::printf("key_opaque_null=%d\n", key2.isNull() ? 1 : 0); return 0; }
