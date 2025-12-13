#ifndef AFPLOGINWIDGET_H
#define AFPLOGINWIDGET_H

#include <QWidget>
#include <QLineEdit>
#include <QPushButton>
#include <QListWidget>
#include <QLabel>
#include <QComboBox>

class AfpLoginWidget : public QWidget {
    Q_OBJECT
public:
    AfpLoginWidget(QWidget *parent = nullptr);
    ~AfpLoginWidget();

    QLineEdit *username;
    QLineEdit *password;
    QPushButton *connect_button;
    QPushButton *disconnect;
    QPushButton *attach;
    QPushButton *detach;
    QListWidget *volume_list;
    QLabel *status_line;
    QLabel *login_message;
    QComboBox *authentication;
};

#endif // AFPLOGINWIDGET_H
