/*
    SPDX-FileCopyrightText: 2024 Hy Murveit <hy@murveit.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "ui_imagingplanner.h"
#include "catalogsdb.h"
#include <QPointer>
#include <QDialog>
#include <QDir>
#include <QFuture>
#include <QFutureWatcher>
#include <QSortFilterProxyModel>
#include <QMenu>

class QStandardItemModel;
class QStandardItem;
class ImagingPlannerPopup;
class KSMoon;

// These are used to communicate with the database.
class ImagingPlannerDBEntry
{
public:
    enum EntryFlag
    {
        PickedBit  = 0x1,
        ImagedBit  = 0x2,
        AddedBit   = 0x4,
        IgnoredBit = 0x8
    };

    QString m_Name;
    int m_Flags;
    QString m_Notes;
    ImagingPlannerDBEntry() {};
    ImagingPlannerDBEntry(const QString &name, int flags, const QString &notes);
    ImagingPlannerDBEntry(const QString &name, bool picked, bool imaged, bool added, bool ignored, const QString &notes);
    void setFlags(bool picked, bool imaged, bool added, bool ignored);
    void getFlags(bool *picked, bool *imaged, bool *added, bool *ignored);
    QString name() { return m_Name; }
};

class ImagingPlannerUI : public QFrame, public Ui::ImagingPlanner
{
    Q_OBJECT

  public:
    explicit ImagingPlannerUI(QWidget *parent);
};

class CatalogImageInfo
{
  public:
    CatalogImageInfo() {}
    CatalogImageInfo(const QString &csv);
    QString m_Name, m_Filename, m_Author, m_Link, m_License;
    const static int MIN_COLUMNS = 5;
};

class CatalogFilter : public QSortFilterProxyModel
{
        Q_OBJECT
    public:
        CatalogFilter(QObject* parent = 0);
        bool filterAcceptsRow(int row, const QModelIndex &parent) const override;
        bool lessThan ( const QModelIndex & left, const QModelIndex & right ) const override;
        void setMinHours(double hours);
        void setImagedConstraints(bool enabled, bool required);
        void setPickedConstraints(bool enabled, bool required);
        void setIgnoredConstraints(bool enabled, bool required);
        void setAddedConstraints(bool enabled, bool required);
        void setKeywordConstraints(bool enabled, bool required, const QString &keyword);
        void setSortColumn(int column);

private:
        double m_MinHours = 0;
        bool m_ImagedConstraintsEnabled = false;
        bool m_ImagedRequired = false;
        bool m_PickedConstraintsEnabled = false;
        bool m_PickedRequired = false;
        bool m_IgnoredConstraintsEnabled = false;
        bool m_IgnoredRequired = false;
        bool m_AddedConstraintsEnabled = false;
        bool m_AddedRequired = false;
        bool m_KeywordConstraintsEnabled = false;
        bool m_KeywordRequired = false;
        QString m_Keyword;
        QRegularExpression m_KeywordRE;
        int m_SortColumn = 1;  // HOURS
        bool m_ReverseSort = false;
};

class ImagingPlanner : public QDialog
{
    Q_OBJECT

  public:
    ImagingPlanner();
    virtual ~ImagingPlanner() override = default;

    bool eventFilter(QObject *obj, QEvent *event) override;

  public slots:
    void selectionChanged(const QItemSelection &selected, const QItemSelection &deselected);
    void setSelection(int flag, bool enabled);

    void setSelectionIgnored();
    void setSelectionNotIgnored();

    void setSelectionImaged();
    void setSelectionNotImaged();

    void setSelectionPicked();
    void setSelectionNotPicked();

    void centerOnSkymap();

protected slots:
    void slotClose();
    void searchAstrobin();
    void searchWikipedia();
    void searchSimbad();
    void searchNGCICImages();
    void recomputeHours();
    void addUserObject();
    void sorry(const QString &message);

signals:
    void popupSorry(const QString &message);
    void addRow(QList<QStandardItem *> itemList);

  protected:
    void showEvent(QShowEvent *) override;
    void resizeEvent(QResizeEvent *) override;

  private slots:
    void userNotesEditFinished();
    void keywordEditFinished();
    void loadImagedFile();
    void searchSlot();
    void loadCatalogViaMenu();
    void getHelp();
    void addRowSlot(QList<QStandardItem *> itemList);


  private:
    void initialize();
    void catalogLoaded(bool addUserAddedObjects = true);
    void loadCatalog(const QString &path, bool addUserAddedObjects);


    // Methods for setting up buttons and options.
    void setupHideButtons(bool(*option)(), void(*setOption)(bool),
                          QPushButton *hideButton, QPushButton *showButton,
                          QFrame *widget, QFrame *hiddenWidget);
    void setupFilterButton(QCheckBox *checkbox, bool(*option)(), void(*setOption)(bool));
    void setupFilter2Buttons(
            QCheckBox *yes, QCheckBox *no, QCheckBox *dontCare,
            bool(*yesOption)(), bool(*noOption)(), bool(*dontCareOption)(),
            void(*setYesOption)(bool), void(*setNoOption)(bool), void(*setDontCareOption)(bool));
    void updateSortConstraints();

    GeoLocation *getGeo();
    QDate getDate() const;

    void loadInitialCatalog();
    QString defaultDirectory() const;
    QString findDefaultCatalog() const;
    bool getKStarsCatalogObject(const QString &name, CatalogObject *catObject, bool useNameResolver = false);
    bool addCatalogItem(const QString &name, int flags = 0, bool useNameResolver = false);
    QUrl getAstrobinUrl(const QString &target, bool requireAwards, bool requireSomeFilters, double minRadius, double maxRadius);
    void popupAstrobin(const QString &target);
    void plotAltitudeGraph(const QDate &date, const dms &ra, const dms &dec);

    void moveBackOneDay();
    void moveForwardOneDay();

    void updateStatus(const QString &message);
    void standardStatus();

    void updateDetails(const CatalogObject &object, int flags);
    void updateNotes(const QString &notes);
    void initUserNotes();
    void disableUserNotes();
    void setupNotesLinks(const QString &notes);

    void setDefaultImage();

    QString currentObjectName();
    int currentObjectFlags();
    QString currentObjectNotes();
    void setCurrentObjectNotes(const QString &notes);

    CatalogObject *currentCatalogObject();
    CatalogObject *getObject(const QString &name);
    CatalogObject *addObject(const QString &name, bool useNameResolver = false);
    void clearObjects();

    void loadCatalogFromFile(QString filename = "", bool reset=true);
    bool findCatalogImageInfo(const QString &name, CatalogImageInfo *info);
    void addCatalogImageInfo(const CatalogImageInfo &info);

    void objectDetails();
    void updateDisplays();
    void updateCounts();
    KSMoon *getMoon();
    void updateMoon();

    // Database utilities.
    void saveToDB(const QString &name, bool picked, bool imaged, bool added, bool ignored, const QString &notes);
    void saveToDB(const QString &name, int flags, const QString &notes);
    void loadFromDB(bool addUserAddedObjects = true);

    void highlightImagedObject(const QModelIndex &index, bool imaged);

    void focusOnTable();
    void adjustWindowSize();

    // Used for debugging the object lists.
    void checkTargets();

    ImagingPlannerUI *ui { nullptr };

    bool m_initialResultsLoad { false };
    bool m_InitialLoad = true;

    CatalogsDB::DBManager m_manager;
    QPointer<QStandardItemModel> m_CatalogModel;
    QPointer<CatalogFilter> m_CatalogSortModel;

    QFuture<void> m_LoadCatalogs;
    QFutureWatcher<void> *m_LoadCatalogsWatcher;

    QHash<QString, CatalogObject> m_CatalogHash;
    QPixmap m_NoImagePixmap;

    QPointer<ImagingPlannerPopup> m_PopupMenu;

    double m_MinMoon = 30.0;
    double m_MinAltitude = 30.0;
    double m_MinHours = 0;
    bool m_UseArtificialHorizon = true;
    QString m_Keyword;

    int m_numWithImage = 0;
    int m_numMissingImage = 0;

    QMap<QString, CatalogImageInfo> m_CatalogImageInfoMap;
};

class ImagingPlannerPopup : public QMenu
{
    Q_OBJECT
  public:
    ImagingPlannerPopup();
    virtual ~ImagingPlannerPopup() override = default;

    void init(ImagingPlanner *planner, const QStringList &names,
              const bool *imaged, const bool *picked, const bool *ignored);
};

