#include "afploginwidget.h"
#include <QVBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QListWidget>
#include <KLocalizedString>

AfpLoginWidget::AfpLoginWidget(QWidget *parent)
    : QWidget(parent)
{
    QVBoxLayout *layout = new QVBoxLayout(this);

    QLabel *userLabel = new QLabel(i18nc("label: username field in login dialog", "Username:"), this);
    username = new QLineEdit(this);
    layout->addWidget(userLabel);
    layout->addWidget(username);

    QLabel *passLabel = new QLabel(i18nc("label: password field in login dialog", "Password:"), this);
    password = new QLineEdit(this);
    password->setEchoMode(QLineEdit::Password);
    layout->addWidget(passLabel);
    layout->addWidget(password);

    connect_button = new QPushButton(i18nc("action:button connect to server", "Connect"), this);
    disconnect = new QPushButton(i18nc("action:button disconnect from server", "Disconnect"), this);
    disconnect->setEnabled(false);
    layout->addWidget(connect_button);
    layout->addWidget(disconnect);

    attach = new QPushButton(i18nc("action:button attach to volume", "Attach"), this);
    attach->setEnabled(false);
    detach = new QPushButton(i18nc("action:button detach from volume", "Detach"), this);
    detach->setEnabled(false);
    layout->addWidget(attach);
    layout->addWidget(detach);

    volume_list = new QListWidget(this);
    volume_list->setEnabled(false);
    layout->addWidget(volume_list);

    status_line = new QLabel(i18n("Ready"), this);
    layout->addWidget(status_line);

    login_message = new QLabel(this);
    login_message->setEnabled(false);
    layout->addWidget(login_message);

    authentication = new QComboBox(this);
    layout->addWidget(authentication);

    setLayout(layout);
}

AfpLoginWidget::~AfpLoginWidget()
{
}
