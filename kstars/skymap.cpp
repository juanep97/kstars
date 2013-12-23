/**************************************************************************
                          skymap.cpp  -  K Desktop Planetarium
                             -------------------
    begin                : Sat Feb 10 2001
    copyright            : (C) 2001 by Jason Harris
    email                : jharris@30doradus.org
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#ifdef _WIN32
#include <windows.h>
#endif
#include "skymap.h"

#include <cmath>

#include <QCursor>
#include <QBitmap>
#include <QPainter>
#include <QPixmap>
#include <QToolTip>
#include <QTextStream>
#include <QFile>
#include <QPointF>
#include <QApplication>
#include <QGraphicsScene>
#include <QInputDialog>

#include <kactioncollection.h>
#include <kconfig.h>
#include <kiconloader.h>
#include <kstatusbar.h>
#include <kmessagebox.h>
#include <kaction.h>
#include <kstandarddirs.h>
#include <ktoolbar.h>
#include <ktoolinvocation.h>
#include <kicon.h>

#include "Options.h"
#include "kstars.h"
#include "kstarsdata.h"
#include "ksutils.h"
#include "imageviewer.h"
#include "dialogs/detaildialog.h"
#include "kspopupmenu.h"
#include "printing/printingwizard.h"
#include "simclock.h"
#include "skyobjects/skyobject.h"
#include "skyobjects/deepskyobject.h"
#include "skyobjects/ksplanetbase.h"
#include "skycomponents/skymapcomposite.h"
#include "skycomponents/flagcomponent.h"
#include "widgets/infoboxwidget.h"
#include "projections/projector.h"
#include "projections/lambertprojector.h"
#include "projections/gnomonicprojector.h"
#include "projections/stereographicprojector.h"
#include "projections/orthographicprojector.h"
#include "projections/azimuthalequidistantprojector.h"
#include "projections/equirectangularprojector.h"
#include "fov.h"

#include "tools/flagmanager.h"

#include "texturemanager.h"

#include "skymapqdraw.h"

#ifdef HAVE_OPENGL
#include "skymapgldraw.h"
#endif

// DEBUG TODO: Remove later -- required to display starhop results for debug.
#include "skycomponents/targetlistcomponent.h"

#include "starhopper.h"

#ifdef HAVE_XPLANET
#include <KProcess>
#include <kfiledialog.h>
#endif

namespace {

    // Draw bitmap for zoom cursor. Width is size of pen to draw with.
    QBitmap zoomCursorBitmap(int width) {
        QBitmap b(32, 32);
        b.fill(Qt::color0);
        int mx = 16, my = 16;
        // Begin drawing
        QPainter p;
        p.begin( &b );
          p.setPen( QPen( Qt::color1, width ) );
          p.drawEllipse( mx - 7, my - 7, 14, 14 );
          p.drawLine(    mx + 5, my + 5, mx + 11, my + 11 );
        p.end();
        return b;
    }

    // Draw bitmap for default cursor. Width is size of pen to draw with.
    QBitmap defaultCursorBitmap(int width) {
        QBitmap b(32, 32);
        b.fill(Qt::color0);
        int mx = 16, my = 16;
        // Begin drawing
        QPainter p;
        p.begin( &b );
          p.setPen( QPen( Qt::color1, width ) );
          // 1. diagonal
          p.drawLine (mx - 2, my - 2, mx - 8, mx - 8);
          p.drawLine (mx + 2, my + 2, mx + 8, mx + 8);
          // 2. diagonal
          p.drawLine (mx - 2, my + 2, mx - 8, mx + 8);
          p.drawLine (mx + 2, my - 2, mx + 8, mx - 8);
        p.end();
        return b;
    }
}


SkyMap* SkyMap::pinstance = 0;


SkyMap* SkyMap::Create()
{
    delete pinstance;
    pinstance = new SkyMap();
    return pinstance;
}

SkyMap* SkyMap::Instance( )
{
    return pinstance;
}

SkyMap::SkyMap() : 
    QGraphicsView( KStars::Instance() ),
    computeSkymap(true), rulerMode(false),
    data( KStarsData::Instance() ), pmenu(0),
    ClickedObject(0), FocusObject(0), m_proj(0),
    m_previewLegend(false), m_objPointingMode(false)
{
    m_Scale = 1.0;

    ZoomRect = QRect();

    setDefaultMouseCursor();	// set the cross cursor

    QPalette p = palette();
    p.setColor( QPalette::Window, QColor( data->colorScheme()->colorNamed( "SkyColor" ) ) );
    setPalette( p );

    setFocusPolicy( Qt::StrongFocus );
    setMinimumSize( 380, 250 );
    setSizePolicy( QSizePolicy( QSizePolicy::Expanding, QSizePolicy::Expanding ) );
    setHorizontalScrollBarPolicy( Qt::ScrollBarAlwaysOff );
    setVerticalScrollBarPolicy( Qt::ScrollBarAlwaysOff );
    setStyleSheet( "QGraphicsView { border-style: none; }" ); 

    setMouseTracking (true); //Generate MouseMove events!
    midMouseButtonDown = false;
    mouseButtonDown = false;
    slewing = false;
    clockSlewing = false;

    ClickedObject = NULL;
    FocusObject = NULL;

    m_SkyMapDraw = 0;

    pmenu = new KSPopupMenu();

    setupProjector();

    //Initialize Transient label stuff
    m_HoverTimer.setSingleShot( true ); // using this timer as a single shot timer

    connect( &m_HoverTimer,   SIGNAL( timeout() ), this, SLOT( slotTransientLabel() ) );
    connect( this, SIGNAL( destinationChanged() ), this, SLOT( slewFocus() ) );

    // Time infobox
    m_timeBox = new InfoBoxWidget( Options::shadeTimeBox(),
                                   Options::positionTimeBox(),
                                   Options::stickyTimeBox(),
                                   QStringList(), this);
    m_timeBox->setVisible( Options::showTimeBox() );
    connect(data->clock(), SIGNAL( timeChanged() ),
            m_timeBox,     SLOT(   slotTimeChanged() ) );
    connect(data->clock(), SIGNAL( timeAdvanced() ),
            m_timeBox,     SLOT(   slotTimeChanged() ) );

    // Geo infobox
    m_geoBox = new InfoBoxWidget( Options::shadeGeoBox(),
                                  Options::positionGeoBox(),
                                  Options::stickyGeoBox(),
                                  QStringList(), this);
    m_geoBox->setVisible( Options::showGeoBox() );
    connect(data,     SIGNAL( geoChanged() ),
            m_geoBox, SLOT(   slotGeoChanged() ) );

    // Object infobox
    m_objBox = new InfoBoxWidget( Options::shadeFocusBox(),
                                  Options::positionFocusBox(),
                                  Options::stickyFocusBox(),
                                  QStringList(), this);
    m_objBox->setVisible( Options::showFocusBox() );
    connect(this,     SIGNAL( objectChanged( SkyObject*) ),
            m_objBox, SLOT(   slotObjectChanged( SkyObject*) ) );
    connect(this,     SIGNAL( positionChanged( SkyPoint*) ),
            m_objBox, SLOT(   slotPointChanged(SkyPoint*) ) );

#ifdef HAVE_OPENGL
    m_SkyMapQDraw  = new SkyMapQDraw( this );
    m_SkyMapQDraw->setMouseTracking( true );
    m_SkyMapGLDraw = new SkyMapGLDraw( this );
    m_SkyMapGLDraw->setMouseTracking( true );
    m_SkyMapGLDraw->hide();
    m_SkyMapQDraw->hide();

    if( Options::useGL() )
        m_SkyMapDraw = m_SkyMapGLDraw;
    else
        m_SkyMapDraw = m_SkyMapQDraw;
#else
    m_SkyMapDraw = new SkyMapQDraw( this );
    m_SkyMapDraw->setMouseTracking( true );
#endif
    
    m_SkyMapDraw->setParent( this->viewport() );
    m_SkyMapDraw->show();

    /*
    m_Scene = new QGraphicsScene( rect() );
    setScene( m_Scene );
    */

#ifdef HAVE_OPENGL
    // If GL is enabled, the InfoBoxes work only with native painting.
    m_iboxes = new InfoBoxes( m_SkyMapQDraw );
#else
    m_iboxes = new InfoBoxes( m_SkyMapDraw );
#endif

    m_iboxes->setVisible( Options::showInfoBoxes() );
    m_iboxes->addInfoBox(m_timeBox);
    m_iboxes->addInfoBox(m_geoBox);
    m_iboxes->addInfoBox(m_objBox);
    /*
    ( m_Scene->addWidget( m_iboxes ) )->setAcceptedMouseButtons( Qt::NoButton );
    */


    //The update timer will be destructed when SkyMap is..
    QTimer *update = new QTimer(this);
    update->setInterval(30);
    connect(update, SIGNAL(timeout()), this, SLOT(update()) );
    update->start();

}

void SkyMap::slotToggleGeoBox(bool flag) {
    m_geoBox->setVisible(flag);
}

void SkyMap::slotToggleFocusBox(bool flag) {
    m_objBox->setVisible(flag);
}

void SkyMap::slotToggleTimeBox(bool flag) {
    m_timeBox->setVisible(flag);
}

void SkyMap::slotToggleInfoboxes(bool flag) {
    m_iboxes->setVisible(flag);
}

SkyMap::~SkyMap() {
    /* == Save infoxes status into Options == */
    Options::setShowInfoBoxes( m_iboxes->isVisibleTo( parentWidget() ) );
    // Time box
    Options::setPositionTimeBox( m_timeBox->pos() );
    Options::setShadeTimeBox(    m_timeBox->shaded() );
    Options::setStickyTimeBox(   m_timeBox->sticky() );
    Options::setShowTimeBox(     m_timeBox->isVisibleTo(m_iboxes) );
    // Geo box
    Options::setPositionGeoBox( m_geoBox->pos() );
    Options::setShadeGeoBox(    m_geoBox->shaded() );
    Options::setStickyGeoBox(   m_geoBox->sticky() );
    Options::setShowGeoBox(     m_geoBox->isVisibleTo(m_iboxes) );
    // Obj box
    Options::setPositionFocusBox( m_objBox->pos() );
    Options::setShadeFocusBox(    m_objBox->shaded() );
    Options::setStickyFocusBox(   m_objBox->sticky() );
    Options::setShowFocusBox(     m_objBox->isVisibleTo(m_iboxes) );
    
    //store focus values in Options
    //If not tracking and using Alt/Az coords, stor the Alt/Az coordinates
    if ( Options::useAltAz() && ! Options::isTracking() ) {
        Options::setFocusRA(  focus()->az().Degrees() );
        Options::setFocusDec( focus()->alt().Degrees() );
    } else {
        Options::setFocusRA(  focus()->ra().Hours() );
        Options::setFocusDec( focus()->dec().Degrees() );
    }

#ifdef HAVE_OPENGL
    delete m_SkyMapGLDraw;
    delete m_SkyMapQDraw;
    m_SkyMapDraw = 0; // Just a formality
#else
    delete m_SkyMapDraw;
#endif

    delete pmenu;

    delete m_proj;

    pinstance = 0;
}

void SkyMap::showFocusCoords() {
    if( focusObject() && Options::isTracking() )
        emit objectChanged( focusObject() );
    else
        emit positionChanged( focus() );
}

void SkyMap::slotTransientLabel() {
    //This function is only called if the HoverTimer manages to timeout.
    //(HoverTimer is restarted with every mouseMoveEvent; so if it times
    //out, that means there was no mouse movement for HOVER_INTERVAL msec.)
    if ( ! slewing && ! ( Options::useAltAz() && Options::showGround() &&
                          SkyPoint::refract(m_MousePoint.alt()).Degrees() < 0.0 ) ) {
        double maxrad = 1000.0/Options::zoomFactor();
        SkyObject *so = data->skyComposite()->objectNearest( &m_MousePoint, maxrad );

        if ( so && ! isObjectLabeled( so ) ) {
            QToolTip::showText(
                QCursor::pos(),
                i18n("%1: %2<sup>m</sup>",
                     so->translatedLongName(),
                     QString::number(so->mag(), 'f', 1)),
                this);
        }
    }
}


//Slots

void SkyMap::setClickedObject( SkyObject *o ) {
	  ClickedObject = o;
}

void SkyMap::setFocusObject( SkyObject *o ) {
    FocusObject = o;
    if ( FocusObject )
        Options::setFocusObject( FocusObject->name() );
    else
        Options::setFocusObject( i18n( "nothing" ) );
}

void SkyMap::slotCenter() {
    KStars* kstars = KStars::Instance();
    TrailObject* trailObj = dynamic_cast<TrailObject*>( focusObject() );
    
    setFocusPoint( clickedPoint() );
    if ( Options::useAltAz() )
        focusPoint()->EquatorialToHorizontal( data->lst(), data->geo()->lat() );

    //clear the planet trail of old focusObject, if it was temporary
    if( trailObj && data->temporaryTrail ) {
        trailObj->clearTrail();
        data->temporaryTrail = false;
    }

    //If the requested object is below the opaque horizon, issue a warning message
    //(unless user is already pointed below the horizon)
    if ( Options::useAltAz() && Options::showGround() &&
            focus()->alt().Degrees() > -1.0 && focusPoint()->alt().Degrees() < -1.0 ) {

        QString caption = i18n( "Requested Position Below Horizon" );
        QString message = i18n( "The requested position is below the horizon.\nWould you like to go there anyway?" );
        if ( KMessageBox::warningYesNo( this, message, caption,
                                        KGuiItem(i18n("Go Anyway")), KGuiItem(i18n("Keep Position")), "dag_focus_below_horiz" )==KMessageBox::No ) {
            setClickedObject( NULL );
            setFocusObject( NULL );
            Options::setIsTracking( false );

            return;
        }
    }

    //set FocusObject before slewing.  Otherwise, KStarsData::updateTime() can reset
    //destination to previous object...
    setFocusObject( ClickedObject );
    Options::setIsTracking( true );
    if ( kstars ) {
        kstars->actionCollection()->action("track_object")->setIcon( KIcon("document-encrypt") );
        kstars->actionCollection()->action("track_object")->setText( i18n( "Stop &Tracking" ) );
    }

    //If focusObject is a SS body and doesn't already have a trail, set the temporaryTrail

    if( Options::useAutoTrail() && trailObj && trailObj->hasTrail() ) {
        trailObj->addToTrail();
        data->temporaryTrail = true;
    }

    //update the destination to the selected coordinates
    if ( Options::useAltAz() ) {
        setDestinationAltAz( focusPoint()->altRefracted(), focusPoint()->az() );
    } else {
        setDestination( *focusPoint() );
    }

    focusPoint()->EquatorialToHorizontal( data->lst(), data->geo()->lat() );

    //display coordinates in statusBar
    emit mousePointChanged( focusPoint() );
    showFocusCoords(); //update FocusBox
}

void SkyMap::slotDSS() {
    dms ra(0.0), dec(0.0);
    QString urlstring;

    //ra and dec must be the coordinates at J2000.  If we clicked on an object, just use the object's ra0, dec0 coords
    //if we clicked on empty sky, we need to precess to J2000.
    if ( clickedObject() ) {
        urlstring = KSUtils::getDSSURL( clickedObject() );
    } else {
        SkyPoint deprecessedPoint = clickedPoint()->deprecess( data->updateNum() );
        ra  = deprecessedPoint.ra();
        dec = deprecessedPoint.dec();
        urlstring = KSUtils::getDSSURL( ra, dec ); // Use default size for non-objects
    }

    KUrl url ( urlstring );

    KStars* kstars = KStars::Instance();
    if( kstars ) {
        ImageViewer *iv = new ImageViewer( url,
            i18n( "Digitized Sky Survey image provided by the Space Telescope Science Institute [public domain]." ),
            this );
        iv->show();
    }
}

void SkyMap::slotSDSS() {
    // TODO: Remove code duplication -- we have the same stuff
    // implemented in ObservingList::setCurrentImage() etc. in
    // tools/observinglist.cpp; must try to de-duplicate as much as
    // possible.
    QString URLprefix( "http://casjobs.sdss.org/ImgCutoutDR6/getjpeg.aspx?" );
    QString URLsuffix( "&scale=1.0&width=600&height=600&opt=GST&query=SR(10,20)" );
    dms ra(0.0), dec(0.0);
    QString RAString, DecString;

    //ra and dec must be the coordinates at J2000.  If we clicked on an object, just use the object's ra0, dec0 coords
    //if we clicked on empty sky, we need to precess to J2000.
    if ( clickedObject() ) {
        ra  = clickedObject()->ra0();
        dec = clickedObject()->dec0();
    } else {
        SkyPoint deprecessedPoint = clickedPoint()->deprecess( data->updateNum() );
        ra  = deprecessedPoint.ra();
        dec = deprecessedPoint.dec();
    }

    RAString = RAString.sprintf( "ra=%f", ra.Degrees() );
    DecString = DecString.sprintf( "&dec=%f", dec.Degrees() );

    //concat all the segments into the kview command line:
    KUrl url (URLprefix + RAString + DecString + URLsuffix);

    KStars* kstars = KStars::Instance();
    if( kstars ) {
        ImageViewer *iv = new ImageViewer( url,
                                           i18n( "Sloan Digital Sky Survey image provided by the Astrophysical Research Consortium [free for non-commercial use]." ),
                                           this );
        iv->show();
    }
}

void SkyMap::slotBeginAngularDistance() {
    beginRulerMode( false );
}

void SkyMap::slotBeginStarHop() {
    beginRulerMode( true );
}

void SkyMap::beginRulerMode( bool starHopRuler ) {
    rulerMode = true;
    starHopDefineMode = starHopRuler;
    AngularRuler.clear();

    //If the cursor is near a SkyObject, reset the AngularRuler's 
    //start point to the position of the SkyObject
    double maxrad = 1000.0/Options::zoomFactor();
    SkyObject *so = data->skyComposite()->objectNearest( clickedPoint(), maxrad );
    if ( so ) {
        AngularRuler.append( so );
        AngularRuler.append( so );
    } else {
        AngularRuler.append( clickedPoint() );
        AngularRuler.append( clickedPoint() );
    }

    AngularRuler.update( data );
}

void SkyMap::slotEndRulerMode() {
    if( !rulerMode )
        return;
    if( !starHopDefineMode ) { // Angular Ruler
        QString sbMessage;

        //If the cursor is near a SkyObject, reset the AngularRuler's
        //end point to the position of the SkyObject
        double maxrad = 1000.0/Options::zoomFactor();
        SkyObject *so = data->skyComposite()->objectNearest( clickedPoint(), maxrad );
        if ( so ) {
            AngularRuler.setPoint( 1, so );
            sbMessage = so->translatedLongName() + "   ";
        } else {
            AngularRuler.setPoint( 1, clickedPoint() );
        }

        rulerMode=false;
        AngularRuler.update( data );
        dms angularDistance = AngularRuler.angularSize();
        AngularRuler.clear();

        sbMessage += i18n( "Angular distance: %1", angularDistance.toDMSString() );

        // Create unobsructive message box with suicidal tendencies
        // to display result.
        InfoBoxWidget* box = new InfoBoxWidget(
            true, mapFromGlobal( QCursor::pos() ), 0, QStringList(sbMessage), this);
        connect(box, SIGNAL( clicked() ), box, SLOT( deleteLater() ));
        QTimer::singleShot(5000, box, SLOT( deleteLater() ));
        box->adjust();
        box->show();
    }
    else { // Star Hop
        StarHopper hopper;
        const SkyPoint &startHop = *AngularRuler.point( 0 );
        const SkyPoint &stopHop = *clickedPoint();
        double fov; // Field of view in arcminutes
        bool ok; // true if user did not cancel the operation
        if( data->getVisibleFOVs().size() == 1 ) {
            // Exactly 1 FOV symbol visible, so use that. Also assume a circular FOV of size min{sizeX, sizeY}
            FOV *f = data->getVisibleFOVs().first();
            fov = ( ( f->sizeX() >= f->sizeY() && f->sizeY() != 0 ) ? f->sizeY() : f->sizeX() );
            ok = true;
        }
        else if( !data->getVisibleFOVs().isEmpty() ) {
            // Ask the user to choose from a list of available FOVs.
            FOV const *f;
            QMap< QString, double > nameToFovMap;
            foreach( f, data->getVisibleFOVs() ) {
                nameToFovMap.insert( f->name(), ( ( f->sizeX() >= f->sizeY() && f->sizeY() != 0) ? f->sizeY() : f->sizeX() ) );
            }
            fov = nameToFovMap[ QInputDialog::getItem( this, i18n("Star Hopper: Choose a field-of-view"), i18n("FOV to use for star hopping:"), nameToFovMap.uniqueKeys(), 0, false, &ok ) ];
        }
        else {
            // Ask the user to enter a field of view
            fov = QInputDialog::getDouble( this, i18n("Star Hopper: Enter field-of-view to use"), i18n("FOV to use for star hopping (in arcminutes):"), 60.0, 1.0, 600.0, 1, &ok );
        }

        Q_ASSERT( fov > 0.0 );

        if( ok ) {

            kDebug() << "fov = " << fov;

            QList<const StarObject *> path = hopper.computePath( startHop, stopHop, fov/60.0, 9.0 ); // FIXME: Hardcoded magnitude limits for testing

            QList<SkyObject *> *mutablestarlist = new QList<SkyObject *>(); // FIXME: Memory leak
            kDebug() << "path count: " << path.count();
            foreach( const StarObject *conststar, path ) {
                StarObject *mutablestar = const_cast<StarObject *>(conststar); // FIXME: Ugly const_cast
                mutablestarlist->append( mutablestar );
                kDebug() << "Added star!";
            }

            TargetListComponent *t = KStarsData::Instance()->skyComposite()->getStarHopRouteList();
            delete t->list;
            t->list = mutablestarlist;
        }

        rulerMode = false;
    }

}

void SkyMap::slotCancelRulerMode(void) {
    rulerMode = false;
    AngularRuler.clear();
}

void SkyMap::slotAddFlag() {
    KStars *ks = KStars::Instance();

    // popup FlagManager window and update coordinates
    ks->slotFlagManager();
    ks->getFlagManager()->clearFields();
    ks->getFlagManager()->setRaDec( clickedPoint()->ra(), clickedPoint()->dec() );
}

void SkyMap::slotEditFlag( int flagIdx ) {
    KStars *ks = KStars::Instance();

    // popup FlagManager window and switch to selected flag
    ks->slotFlagManager();
    ks->getFlagManager()->showFlag( flagIdx );
}

void SkyMap::slotDeleteFlag( int flagIdx ) {
    KStars *ks = KStars::Instance();

    ks->data()->skyComposite()->flags()->remove( flagIdx );
    ks->data()->skyComposite()->flags()->saveToFile();

    // if there is FlagManager created, update its flag model
    if ( ks->getFlagManager() ) {
        ks->getFlagManager()->deleteFlagItem( flagIdx );
    }
}

void SkyMap::slotImage() {
    QString message = ((KAction*)sender())->text();
    message = message.remove( '&' ); //Get rid of accelerator markers

    // Need to do this because we are comparing translated strings
    int index = -1;
    for( int i = 0; i < clickedObject()->ImageTitle().size(); ++i ) {
        if( i18nc( "Image/info menu item (should be translated)", clickedObject()->ImageTitle().at( i ).toLocal8Bit().data() ) == message ) {
            index = i;
            break;
        }
    }

    QString sURL;
    if ( index >= 0 && index < clickedObject()->ImageList().size() ) {
        sURL = clickedObject()->ImageList()[ index ];
    } else {
        kWarning() << "ImageList index out of bounds: " << index;
        if ( index == -1 ) {
            kWarning() << "Message string \"" << message << "\" not found in ImageTitle.";
            kDebug() << clickedObject()->ImageTitle();
        }
    }

    KUrl url ( sURL );
    if( !url.isEmpty() )
        new ImageViewer( url, clickedObject()->messageFromTitle(message), this );
}

void SkyMap::slotInfo() {
    QString message = ((KAction*)sender())->text();
    message = message.remove( '&' ); //Get rid of accelerator markers

    // Need to do this because we are comparing translated strings
    int index = -1;
    for( int i = 0; i < clickedObject()->InfoTitle().size(); ++i ) {
        if( i18nc( "Image/info menu item (should be translated)", clickedObject()->InfoTitle().at( i ).toLocal8Bit().data() ) == message ) {
            index = i;
            break;
        }
    }

    QString sURL;
    if ( index >= 0 && index < clickedObject()->InfoList().size() ) {
        sURL = clickedObject()->InfoList()[ index ];
    } else {
        kWarning() << "InfoList index out of bounds: " << index;
        if ( index == -1 ) {
            kWarning() << "Message string \"" << message << "\" not found in InfoTitle.";
            kDebug() << clickedObject()->InfoTitle();
        }
    }

    KUrl url ( sURL );
    if (!url.isEmpty())
        KToolInvocation::invokeBrowser(sURL);
}

bool SkyMap::isObjectLabeled( SkyObject *object ) {
    return data->skyComposite()->labelObjects().contains( object );
}

SkyPoint SkyMap::getCenterPoint()
{
    SkyPoint retVal;
    // FIXME: subtraction of these 0.00001 is a simple workaround, because wrong SkyPoint is returned when _exact_ center of
    // SkyMap is passed to the projector.
    retVal = projector()->fromScreen( QPointF(width() / 2 - 0.00001, height() / 2 - 0.00001), data->lst(), data->geo()->lat() );
    return retVal;
}

void SkyMap::slotRemoveObjectLabel() {
    data->skyComposite()->removeNameLabel( clickedObject() );
    forceUpdate();
}

void SkyMap::slotAddObjectLabel() {
    data->skyComposite()->addNameLabel( clickedObject() );
    forceUpdate();
}

void SkyMap::slotRemovePlanetTrail() {
    TrailObject* tobj = dynamic_cast<TrailObject*>( clickedObject() );
    if( tobj ) {
        tobj->clearTrail();
        forceUpdate();
    }
}

void SkyMap::slotAddPlanetTrail() {
    TrailObject* tobj = dynamic_cast<TrailObject*>( clickedObject() );
    if( tobj ) {
        tobj->addToTrail();
        forceUpdate();
    }
}

void SkyMap::slotDetail() {
    // check if object is selected
    if ( !clickedObject() ) {
        KMessageBox::sorry( this, i18n("No object selected."), i18n("Object Details") );
        return;
    }
    DetailDialog* detail = new DetailDialog( clickedObject(), data->ut(), data->geo(), KStars::Instance() );
    detail->setAttribute(Qt::WA_DeleteOnClose);
    detail->show();
}

void SkyMap::slotObjectSelected() {
    if(m_objPointingMode && KStars::Instance()->getPrintingWizard()) {
        KStars::Instance()->getPrintingWizard()->pointingDone(clickedObject());
        m_objPointingMode = false;
    }
}

void SkyMap::slotCancelLegendPreviewMode() {
    m_previewLegend = false;
    forceUpdate(true);
    KStars::Instance()->showImgExportDialog();
}

void SkyMap::slotFinishFovCaptureMode() {
    if(m_fovCaptureMode && KStars::Instance()->getPrintingWizard()) {
        KStars::Instance()->getPrintingWizard()->fovCaptureDone();
        m_fovCaptureMode = false;
    }
}

void SkyMap::slotCaptureFov() {
    if(KStars::Instance()->getPrintingWizard()) {
        KStars::Instance()->getPrintingWizard()->captureFov();
    }
}

void SkyMap::slotClockSlewing() {
    //If the current timescale exceeds slewTimeScale, set clockSlewing=true, and stop the clock.
    if( (fabs( data->clock()->scale() ) > Options::slewTimeScale())  ^  clockSlewing ) {
        data->clock()->setManualMode( !clockSlewing );
        clockSlewing = !clockSlewing;
        // don't change automatically the DST status
        KStars* kstars = KStars::Instance();
        if( kstars )
            kstars->updateTime( false );
    }
}

void SkyMap::setFocus( SkyPoint *p ) {
    setFocus( p->ra(), p->dec() );
}

void SkyMap::setFocus( const dms &ra, const dms &dec ) {
    Options::setFocusRA(  ra.Hours() );
    Options::setFocusDec( dec.Degrees() );

    focus()->set( ra, dec );
    focus()->EquatorialToHorizontal( data->lst(), data->geo()->lat() );
}

void SkyMap::setFocusAltAz( const dms &alt, const dms &az) {
    Options::setFocusRA( focus()->ra().Hours() );
    Options::setFocusDec( focus()->dec().Degrees() );
    focus()->setAlt(alt);
    focus()->setAz(az);
    focus()->HorizontalToEquatorial( data->lst(), data->geo()->lat() );

    slewing = false;
    forceUpdate(); //need a total update, or slewing with the arrow keys doesn't work.
}

void SkyMap::setDestination( const SkyPoint& p ) {
    setDestination( p.ra(), p.dec() );
}

void SkyMap::setDestination( const dms &ra, const dms &dec ) {
    destination()->set( ra, dec );
    destination()->EquatorialToHorizontal( data->lst(), data->geo()->lat() );
    emit destinationChanged();
}

void SkyMap::setDestinationAltAz( const dms &alt, const dms &az) {
    destination()->setAlt(alt);
    destination()->setAz(az);
    destination()->HorizontalToEquatorial( data->lst(), data->geo()->lat() );
    emit destinationChanged();
}

void SkyMap::setClickedPoint( SkyPoint *f ) { 
    ClickedPoint = *f;
}

void SkyMap::updateFocus() {
    if( slewing )
        return;

    //Tracking on an object
    if ( Options::isTracking() && focusObject() != NULL ) {
        if ( Options::useAltAz() ) {
            //Tracking any object in Alt/Az mode requires focus updates
            focusObject()->EquatorialToHorizontal(data->lst(), data->geo()->lat());
            setFocusAltAz( focusObject()->altRefracted(), focusObject()->az() );
            focus()->HorizontalToEquatorial( data->lst(), data->geo()->lat() );
            setDestination( *focus() );
        } else {
            //Tracking in equatorial coords
            setFocus( focusObject() );
            focus()->EquatorialToHorizontal( data->lst(), data->geo()->lat() );
            setDestination( *focus() );
        }

    //Tracking on empty sky
    } else if ( Options::isTracking() && focusPoint() != NULL ) {
        if ( Options::useAltAz() ) {
            //Tracking on empty sky in Alt/Az mode
            setFocus( focusPoint() );
            focus()->EquatorialToHorizontal( data->lst(), data->geo()->lat() );
            setDestination( *focus() );
        }

    // Not tracking and not slewing, let sky drift by
    // This means that horizontal coordinates are constant.
    } else {
        focus()->HorizontalToEquatorial(data->lst(), data->geo()->lat() );
    }
}

void SkyMap::slewFocus() {
    //Don't slew if the mouse button is pressed
    //Also, no animated slews if the Manual Clock is active
    //08/2002: added possibility for one-time skipping of slew with snapNextFocus
    if ( !mouseButtonDown ) {
        bool goSlew =  ( Options::useAnimatedSlewing() && ! data->snapNextFocus() ) &&
                      !( data->clock()->isManualMode() && data->clock()->isActive() );
        if ( goSlew  ) {
            double dX, dY;
            double maxstep = 10.0;
            if ( Options::useAltAz() ) {
                dX = destination()->az().Degrees() - focus()->az().Degrees();
                dY = destination()->alt().Degrees() - focus()->alt().Degrees();
            } else {
                dX = destination()->ra().Degrees() - focus()->ra().Degrees();
                dY = destination()->dec().Degrees() - focus()->dec().Degrees();
            }

            //switch directions to go the short way around the celestial sphere, if necessary.
            dX = KSUtils::reduceAngle(dX, -180.0, 180.0);

            double r0 = sqrt( dX*dX + dY*dY );
            if ( r0 < 20.0 ) { //smaller slews have smaller maxstep
                maxstep *= (10.0 + 0.5*r0)/20.0;
            }
            double step  = 0.5;
            double r  = r0;
            while ( r > step ) {
                //DEBUG
                kDebug() << step << ": " << r << ": " << r0 << endl;
                double fX = dX / r;
                double fY = dY / r;

                if ( Options::useAltAz() ) {
                    focus()->setAlt( focus()->alt().Degrees() + fY*step );
                    focus()->setAz( dms( focus()->az().Degrees() + fX*step ).reduce() );
                    focus()->HorizontalToEquatorial( data->lst(), data->geo()->lat() );
                } else {
                    fX = fX/15.; //convert RA degrees to hours
                    SkyPoint newFocus( focus()->ra().Hours() + fX*step, focus()->dec().Degrees() + fY*step );
                    setFocus( &newFocus );
                    focus()->EquatorialToHorizontal( data->lst(), data->geo()->lat() );
                }

                slewing = true;

                forceUpdate();
                qApp->processEvents(); //keep up with other stuff

                if ( Options::useAltAz() ) {
                    dX = destination()->az().Degrees() - focus()->az().Degrees();
                    dY = destination()->alt().Degrees() - focus()->alt().Degrees();
                } else {
                    dX = destination()->ra().Degrees() - focus()->ra().Degrees();
                    dY = destination()->dec().Degrees() - focus()->dec().Degrees();
                }

                //switch directions to go the short way around the celestial sphere, if necessary.
                dX = KSUtils::reduceAngle(dX, -180.0, 180.0);
                r = sqrt( dX*dX + dY*dY );
                
                //Modify step according to a cosine-shaped profile
                //centered on the midpoint of the slew
                //NOTE: don't allow the full range from -PI/2 to PI/2
                //because the slew will never reach the destination as 
                //the speed approaches zero at the end!
                double t = dms::PI*(r - 0.5*r0)/(1.05*r0);
                step = cos(t)*maxstep;
            }
        }

        //Either useAnimatedSlewing==false, or we have slewed, and are within one step of destination
        //set focus=destination.
        if ( Options::useAltAz() ) {
            setFocusAltAz( destination()->alt(), destination()->az() );
            focus()->HorizontalToEquatorial( data->lst(), data->geo()->lat() );
        } else {
            setFocus( destination() );
            focus()->EquatorialToHorizontal( data->lst(), data->geo()->lat() );
        }

        slewing = false;

        //Turn off snapNextFocus, we only want it to happen once
        if ( data->snapNextFocus() ) {
            data->setSnapNextFocus(false);
        }

        //Start the HoverTimer. if the user leaves the mouse in place after a slew,
        //we want to attach a label to the nearest object.
        if ( Options::useHoverLabel() )
            m_HoverTimer.start( HOVER_INTERVAL );

        forceUpdate();
    }
}

void SkyMap::slotZoomIn() {
    setZoomFactor( Options::zoomFactor() * DZOOM );
}

void SkyMap::slotZoomOut() {
    setZoomFactor( Options::zoomFactor() / DZOOM );
}

void SkyMap::slotZoomDefault() {
    setZoomFactor( DEFAULTZOOM );
}

void SkyMap::setZoomFactor(double factor) {
    Options::setZoomFactor(  KSUtils::clamp(factor, MINZOOM, MAXZOOM)  );
    forceUpdate();
    emit zoomChanged();
}

// force a new calculation of the skymap (used instead of update(), which may skip the redraw)
// if now=true, SkyMap::paintEvent() is run immediately, rather than being added to the event queue
// also, determine new coordinates of mouse cursor.
void SkyMap::forceUpdate( bool now )
{
    QPoint mp( mapFromGlobal( QCursor::pos() ) );
    if (! projector()->unusablePoint( mp )) {
        //determine RA, Dec of mouse pointer
        m_MousePoint = projector()->fromScreen( mp, data->lst(), data->geo()->lat() );
    }

    computeSkymap = true;

    // Ensure that stars are recomputed
    data->incUpdateID();

    if( now )
        m_SkyMapDraw->repaint();
    else
        m_SkyMapDraw->update();
    
}

float SkyMap::fov() {
     float diagonalPixels = sqrt(static_cast<double>( width() * width() + height() * height() ));
     return diagonalPixels / ( 2 * Options::zoomFactor() * dms::DegToRad );
}

void SkyMap::setupProjector() {
    //Update View Parameters for projection
    ViewParams p;
    p.focus         = focus();
    p.height        = height();
    p.width         = width();
    p.useAltAz      = Options::useAltAz();
    p.useRefraction = Options::useRefraction();
    p.zoomFactor    = Options::zoomFactor();
    p.fillGround    = Options::showGround();
    //Check if we need a new projector
    if( m_proj && Options::projection() == m_proj->type() )
        m_proj->setViewParams(p);
    else {
        delete m_proj;
        switch( Options::projection() ) {
            case Gnomonic:
                m_proj = new GnomonicProjector(p);
                break;
            case Stereographic:
                m_proj = new StereographicProjector(p);
                break;
            case Orthographic:
                m_proj = new OrthographicProjector(p);
                break;
            case AzimuthalEquidistant:
                m_proj = new AzimuthalEquidistantProjector(p);
                break;
            case Equirectangular:
                m_proj = new EquirectangularProjector(p);
                break;
            case Lambert: default:
                //TODO: implement other projection classes
                m_proj = new LambertProjector(p);
                break;
        }
    }
}

void SkyMap::setZoomMouseCursor()
{
    mouseMoveCursor = false;	// no mousemove cursor
    QBitmap cursor = zoomCursorBitmap(2);
    QBitmap mask   = zoomCursorBitmap(4);
    setCursor( QCursor(cursor, mask) );
}

void SkyMap::setDefaultMouseCursor()
{
    mouseMoveCursor = false;        // no mousemove cursor
    QBitmap cursor = defaultCursorBitmap(2);
    QBitmap mask   = defaultCursorBitmap(3);
    setCursor( QCursor(cursor, mask) );
}

void SkyMap::setMouseMoveCursor()
{
    if (mouseButtonDown)
    {
        setCursor(Qt::SizeAllCursor);	// cursor shape defined in qt
        mouseMoveCursor = true;
    }
}

void SkyMap::updateAngleRuler() {
    if( rulerMode && (!pmenu || !pmenu->isVisible()) )
        AngularRuler.setPoint( 1, &m_MousePoint );
    AngularRuler.update( data );
}

bool SkyMap::isSlewing() const  {
    return (slewing || ( clockSlewing && data->clock()->isActive() ) );
}

#ifdef HAVE_OPENGL
void SkyMap::slotToggleGL() {

    Q_ASSERT( m_SkyMapGLDraw );
    Q_ASSERT( m_SkyMapQDraw );

    m_SkyMapDraw->setParent( 0 );
    m_SkyMapDraw->hide();

    if( Options::useGL() ) {
        // Do NOT use GL
        Options::setUseGL( false );
        m_SkyMapDraw = m_SkyMapQDraw;
        KStars::Instance()->actionCollection()->action( "opengl" )->setText(i18n("Switch to OpenGL backend"));
    }
    else {
        // Use GL
        QString message = i18n("This version of KStars comes with new experimental OpenGL support. Our experience is that OpenGL works "
                               "much faster on machines with hardware acceleration. Would you like to switch to OpenGL painting backends?");

        int result = KMessageBox::warningYesNo( this, message,
                                                i18n("Switch to OpenGL backend"),
                                                KStandardGuiItem::yes(),
                                                KStandardGuiItem::no(),
                                                "dag_opengl_switch" );

        if ( result == KMessageBox::Yes ) {

            KMessageBox::information( this, i18n("Infoboxes will be disabled as they do not work correctly when using OpenGL backends as of this version"),
                                      i18n("Switch to OpenGL backend"),
                                      "dag_opengl_infoboxes" );

            Options::setUseGL( true );

            m_SkyMapDraw = m_SkyMapGLDraw;
            KStars::Instance()->actionCollection()->action( "opengl" )->setText(i18n("Switch to QPainter backend"));
        }
    }
    m_SkyMapDraw->setParent( viewport() );
    m_SkyMapDraw->show();
    m_SkyMapDraw->resize( size() );
}
#endif

#ifdef HAVE_XPLANET
void SkyMap::startXplanet( const QString & outputFile ) {
    QString year, month, day, hour, minute, seconde, fov;

    // If Options::xplanetPath() is empty, return
    if ( Options::xplanetPath().isEmpty() ) {
        KMessageBox::error(0, i18n("Xplanet binary path is empty in config panel."));
        return;
    }

    // Format date
    if ( year.setNum( data->ut().date().year() ).size() == 1 ) year.push_front( '0' );
    if ( month.setNum( data->ut().date().month() ).size() == 1 ) month.push_front( '0' );
    if ( day.setNum( data->ut().date().day() ).size() == 1 ) day.push_front( '0' );
    if ( hour.setNum( data->ut().time().hour() ).size() == 1 ) hour.push_front( '0' );
    if ( minute.setNum( data->ut().time().minute() ).size() == 1 ) minute.push_front( '0' );
    if ( seconde.setNum( data->ut().time().second() ).size() == 1 ) seconde.push_front( '0' );

    // Create xplanet process
    KProcess *xplanetProc = new KProcess;

    // Add some options
    *xplanetProc << Options::xplanetPath()
            << "-body" << clickedObject()->name().toLower() 
            << "-geometry" << Options::xplanetWidth() + 'x' + Options::xplanetHeight()
            << "-date" <<  year + month + day + '.' + hour + minute + seconde
            << "-glare" << Options::xplanetGlare()
            << "-base_magnitude" << Options::xplanetMagnitude()
            << "-light_time"
            << "-window";

    // General options
    if ( ! Options::xplanetTitle().isEmpty() )
        *xplanetProc << "-window_title" << "\"" + Options::xplanetTitle() + "\"";
    if ( Options::xplanetFOV() )
        *xplanetProc << "-fov" << fov.setNum( this->fov() ).replace( '.', ',' );
    if ( Options::xplanetConfigFile() )
        *xplanetProc << "-config" << Options::xplanetConfigFilePath();
    if ( Options::xplanetStarmap() )
        *xplanetProc << "-starmap" << Options::xplanetStarmapPath();
    if ( Options::xplanetArcFile() )
        *xplanetProc << "-arc_file" << Options::xplanetArcFilePath();
    if ( Options::xplanetWait() )
        *xplanetProc << "-wait" << Options::xplanetWaitValue();
    if ( !outputFile.isEmpty() )
        *xplanetProc << "-output" << outputFile << "-quality" << Options::xplanetQuality();

    // Labels
    if ( Options::xplanetLabel() ) {
        *xplanetProc << "-fontsize" << Options::xplanetFontSize()
                << "-color" << "0x" + Options::xplanetColor().mid( 1 )
                << "-date_format" << Options::xplanetDateFormat();

        if ( Options::xplanetLabelGMT() )
            *xplanetProc << "-gmtlabel";
        else
            *xplanetProc << "-label";
        if ( !Options::xplanetLabelString().isEmpty() )
            *xplanetProc << "-label_string" << "\"" + Options::xplanetLabelString() + "\"";
        if ( Options::xplanetLabelTL() )
            *xplanetProc << "-labelpos" << "+15+15";
        else if ( Options::xplanetLabelTR() )
            *xplanetProc << "-labelpos" << "-15+15";
        else if ( Options::xplanetLabelBR() )
            *xplanetProc << "-labelpos" << "-15-15";
        else if ( Options::xplanetLabelBL() )
            *xplanetProc << "-labelpos" << "+15-15";
    }

    // Markers
    if ( Options::xplanetMarkerFile() )
        *xplanetProc << "-marker_file" << Options::xplanetMarkerFilePath();
    if ( Options::xplanetMarkerBounds() )
        *xplanetProc << "-markerbounds" << Options::xplanetMarkerBoundsPath();

    // Position
    if ( Options::xplanetRandom() )
        *xplanetProc << "-random";
    else
        *xplanetProc << "-latitude" << Options::xplanetLatitude() << "-longitude" << Options::xplanetLongitude();

    // Projection
    if ( Options::xplanetProjection() ) {
        switch ( Options::xplanetProjection() ) {
            case 1 : *xplanetProc << "-projection" << "ancient"; break;
            case 2 : *xplanetProc << "-projection" << "azimuthal"; break;
            case 3 : *xplanetProc << "-projection" << "bonne"; break;
            case 4 : *xplanetProc << "-projection" << "gnomonic"; break;
            case 5 : *xplanetProc << "-projection" << "hemisphere"; break;
            case 6 : *xplanetProc << "-projection" << "lambert"; break;
            case 7 : *xplanetProc << "-projection" << "mercator"; break;
            case 8 : *xplanetProc << "-projection" << "mollweide"; break;
            case 9 : *xplanetProc << "-projection" << "orthographic"; break;
            case 10 : *xplanetProc << "-projection" << "peters"; break;
            case 11 : *xplanetProc << "-projection" << "polyconic"; break;
            case 12 : *xplanetProc << "-projection" << "rectangular"; break;
            case 13 : *xplanetProc << "-projection" << "tsc"; break;
            default : break;
        }
        if ( Options::xplanetBackground() ) {
            if ( Options::xplanetBackgroundImage() )
                *xplanetProc << "-background" << Options::xplanetBackgroundImagePath();
            else
                *xplanetProc << "-background" << "0x" + Options::xplanetBackgroundColorValue().mid( 1 );
        }
    }

    // We add this option at the end otherwise it does not work (???)
    *xplanetProc << "-origin" << "earth";

    // Run xplanet
    kDebug() << "Run:" << xplanetProc->program().join(" ");
    xplanetProc->start();
}

void SkyMap::slotXplanetToScreen() {
    startXplanet();
}

void SkyMap::slotXplanetToFile() {
    QString filename = KFileDialog::getSaveFileName( );
    if ( ! filename.isEmpty() ) {
        startXplanet( filename );
    }
}
#endif

#include "skymap.moc"
