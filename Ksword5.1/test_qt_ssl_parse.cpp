#include <QtCore/QCoreApplication>
#include <QtCore/QFile>
#include <QtCore/QByteArray>
#include <QtCore/QDebug>
#include <QtNetwork/QSslCertificate>
#include <QtNetwork/QSslKey>
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 3) {
        qDebug() << "usage";
        return 2;
    }
    QFile certFile(QString::fromLocal8Bit(argv[1]));
    QFile keyFile(QString::fromLocal8Bit(argv[2]));
    if (!certFile.open(QIODevice::ReadOnly) || !keyFile.open(QIODevice::ReadOnly)) {
        qDebug() << "open_failed" << certFile.errorString() << keyFile.errorString();
        return 3;
    }
    QByteArray certBytes = certFile.readAll();
    QByteArray keyBytes = keyFile.readAll();
    QSslCertificate cert(certBytes, QSsl::Pem);
    QSslKey key(keyBytes, QSsl::Rsa, QSsl::Pem, QSsl::PrivateKey);
    qDebug() << "cert_null" << cert.isNull();
    qDebug() << "key_null" << key.isNull();
    qDebug() << "cert_subject" << cert.subjectInfo(QSslCertificate::CommonName);
    return 0;
}
