#ifndef SETUPWIZARD_H
#define SETUPWIZARD_H

#include <QApplication>
#include <QDialog>
#include <QMessageBox>
#include <QFileDialog>
#include <QDesktopServices>
#include <QPropertyAnimation>

#include "NodeSelector.h"
#include "Preferences.h"
#include "megaapi.h"
#include "QTMegaRequestListener.h"

namespace Ui {
class SetupWizard;
}

class MegaApplication;
class SetupWizard : public QDialog, public mega::MegaRequestListener
{
    Q_OBJECT

public:
    enum {
        PAGE_INITIAL = 0,
        PAGE_NEW_ACCOUNT = 1,
        PAGE_LOGIN = 2,
        PAGE_MODE = 3
    };

    explicit SetupWizard(MegaApplication *app, QWidget *parent = 0);
    ~SetupWizard();

    virtual void onRequestStart(mega::MegaApi* api, mega::MegaRequest *request);
    virtual void onRequestFinish(mega::MegaApi* api, mega::MegaRequest *request, mega::MegaError* e);
    virtual void onRequestUpdate(mega::MegaApi* api, mega::MegaRequest *request);
    void goToStep(int page);
    void initModeSelection();

private slots:
    void on_bNext_clicked();
    void on_bBack_clicked();
    void on_bCancel_clicked();
    void on_bSkip_clicked();
    void on_bLocalFolder_clicked();
    void on_bMegaFolder_clicked();
    void wTypicalSetup_clicked();
    void wAdvancedSetup_clicked();
    void lTermsLink_clicked();
    void on_bLearMore_clicked();
    void on_bFinish_clicked();
    void showErrorMessage(QString error);
    void onErrorAnimationFinished();
    void animationTimout();

    void onPasswordTextChanged(QString text);

private:
    QPropertyAnimation *m_animation;

protected:
    bool eventFilter(QObject *obj, QEvent *event);
    void closeEvent(QCloseEvent * event);
    void setupPreferences();
    void page_login();
    void page_logout();
    void page_mode();
    void page_welcome();
    void page_newaccount();
    void page_progress();

    void setLevelStrength(int level);

    Ui::SetupWizard *ui;
    MegaApplication *app;
    mega::MegaApi *megaApi;
    Preferences *preferences;
    uint64_t selectedMegaFolderHandle;
    QString sessionKey;
    mega::QTMegaRequestListener *delegateListener;
    bool closing;
    bool loggingStarted;
    QTimer *animationTimer;
};

#endif // SETUPWIZARD_H
