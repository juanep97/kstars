/*
    SPDX-FileCopyrightText: 2007 Jason Harris <kstars@30doradus.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "avtplotwidget.h"

#include "kstarsdata.h"
#include "Options.h"

#include <QWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QTime>
#include <QLinearGradient>

#include <KLocalizedString>
#include <kplotobject.h>
#include <kplotpoint.h>
#include <QDebug>

#include "kplotaxis.h"
#include "ksalmanac.h"

AVTPlotWidget::AVTPlotWidget(QWidget *parent) : KPlotWidget(parent)
{
    setAntialiasing(true);

    MousePoint = QPoint(-1, -1);
}

void AVTPlotWidget::mousePressEvent(QMouseEvent *e)
{
    mouseMoveEvent(e);
}

void AVTPlotWidget::mouseDoubleClickEvent(QMouseEvent *)
{
    MousePoint = QPoint(-1, -1);
    update();
}

void AVTPlotWidget::mouseMoveEvent(QMouseEvent *e)
{
    QRect checkRect(leftPadding(), topPadding(), pixRect().width(), pixRect().height());
    int Xcursor = e->x();
    int Ycursor = e->y();

    if (!checkRect.contains(e->x(), e->y()))
    {
        if (e->x() < checkRect.left())
            Xcursor = checkRect.left();
        if (e->x() > checkRect.right())
            Xcursor = checkRect.right();
        if (e->y() < checkRect.top())
            Ycursor = checkRect.top();
        if (e->y() > checkRect.bottom())
            Ycursor = checkRect.bottom();
    }

    Xcursor -= leftPadding();
    Ycursor -= topPadding();

    MousePoint = QPoint(Xcursor, Ycursor);
    update();
}

// All the int coordinates (rise, set) need to be converted from hours relative to midnight
// into graph coordinates before calling this.
void drawMoon(QPainter &p, int rise, int set, int fade, const QColor &color, int width, int height)
{
    QBrush brush(color, Qt::Dense5Pattern);
    QBrush dimmerBrush(color, Qt::Dense6Pattern);
    QBrush dimmestBrush(color, Qt::Dense7Pattern);
    if (set < rise)
    {
        if (set + fade >= 0 && set - fade < width)
        {
            p.fillRect(QRectF(0.0,        0.0, set - fade, height), brush);
            p.fillRect(QRectF(set - fade, 0.0, set,        height), dimmerBrush);
            p.fillRect(QRectF(set,        0.0, set + fade, height), dimmestBrush);
        }
        if (rise + fade >= 0 && rise - fade < width)
        {
            p.fillRect(QRectF(rise - fade, 0.0,   rise,        height), dimmestBrush);
            p.fillRect(QRectF(rise,        0.0,   rise + fade, height), dimmerBrush);
            // Since set < rise, we draw to the end of the box
            p.fillRect(QRectF(rise + fade, 0.0,   width,       height), brush);
        }
    }
    else
    {
        p.fillRect(QRectF(rise - fade, 0.0, rise,        height), dimmestBrush);
        p.fillRect(QRectF(rise,        0.0, rise + fade, height), dimmerBrush);
        p.fillRect(QRectF(rise + fade, 0.0, set - fade,  height), brush);
        p.fillRect(QRectF(set - fade,  0.0, set,         height), dimmerBrush);
        p.fillRect(QRectF(set,         0.0, set + fade,  height), dimmestBrush);

    }
}

// All the int coordinates (rise, set, da, du) need to be converted from hours relative to midnight
// into graph coordinates before calling this.
void drawSun(QPainter &p, int rise, int set, double minAlt, double maxAlt, int da, int du, bool noDawn,
             const QColor &color, int width, int height)
{
    if (maxAlt < 0.0 && minAlt < -18.0)
    {
        // The sun never rise but the sky is not completely dark
        QLinearGradient grad = QLinearGradient(QPointF(0.0, 0.0), QPointF(du, 0.0));

        QColor gradStartColor = color;
        gradStartColor.setAlpha((1 - (maxAlt / -18.0)) * 255);

        grad.setColorAt(0, gradStartColor);
        grad.setColorAt(1, Qt::transparent);
        p.fillRect(QRectF(0.0, 0.0, du, height), grad);
        grad.setStart(QPointF(width, 0.0));
        grad.setFinalStop(QPointF(da, 0.0));
        p.fillRect(QRectF(da, 0.0, width, height), grad);
    }
    else if (maxAlt < 0.0 && minAlt > -18.0)
    {
        // The sun never rise but the sky is NEVER completely dark
        QLinearGradient grad = QLinearGradient(QPointF(0.0, 0.0), QPointF(width, 0.0));

        QColor gradStartEndColor = color;
        gradStartEndColor.setAlpha((1 - (maxAlt / -18.0)) * 255);
        QColor gradMidColor = color;
        gradMidColor.setAlpha((1 - (minAlt / -18.0)) * 255);

        grad.setColorAt(0, gradStartEndColor);
        grad.setColorAt(0.5, gradMidColor);
        grad.setColorAt(1, gradStartEndColor);
        p.fillRect(QRectF(0.0, 0.0, width, height), grad);
    }
    else if (noDawn)
    {
        // The sun sets and rises but the sky is never completely dark
        p.fillRect(0, 0, set, int(0.5 * height), color);
        p.fillRect(rise, 0, width, int(0.5 * height), color);

        QLinearGradient grad = QLinearGradient(QPointF(set, 0.0), QPointF(rise, 0.0));

        QColor gradMidColor = color;
        gradMidColor.setAlpha((1 - (minAlt / -18.0)) * 255);

        grad.setColorAt(0, color);
        grad.setColorAt(0.5, gradMidColor);
        grad.setColorAt(1, color);
        p.fillRect(QRectF(set, 0.0, rise - set, height), grad);
    }
    else
    {
        if (set > 0)
            p.fillRect(0, 0, set, height, color);
        if (rise < width)
            p.fillRect(rise, 0, width, height, color);

        QLinearGradient grad = QLinearGradient(QPointF(set, 0.0), QPointF(du, 0.0));
        grad.setColorAt(0, color);
        grad.setColorAt(1, Qt::transparent);
        p.fillRect(QRectF(set, 0.0, du - set, height), grad);

        grad.setStart(QPointF(rise, 0.0));
        grad.setFinalStop(QPointF(da, 0.0));
        p.fillRect(QRectF(da, 0.0, rise - da, height), grad);
    }
}

// This legacy code always plotted from noon to noon (24 hours starting at noon).
// To generalize this code, we still compute noon-to-noon coords, but then convert
// them to more general plot coordinates where the plot length isn't 24 hours and
// the plot doesn't begin at noon.
int AVTPlotWidget::convertCoords(double xCoord)
{
    const double plotWidth = pixRect().width();
    return plotWidth * ((xCoord * 24.0 / plotWidth) - noonOffset) / plotDuration;
}

namespace
{
double findYValue(const KPlotObject *po, double x)
{
    const auto points = po->points();
    const int size = points.size();
    if (size == 0)                 return 0;
    if (x < points[0]->x())        return points[0]->y();
    if (x > points[size - 1]->x()) return points[size - 1]->y();
    for (int i = 0; i < size - 1; ++i)
    {
        const double ix = points[i]->x();
        const double iy = points[i]->y();
        const double nextIx = points[i + 1]->x();
        const double nextIy = points[i + 1]->y();
        if (x == ix) return iy;
        if (x == nextIx) return nextIy;
        if (x > ix && x < nextIx)
            return iy + (nextIy - iy) * (x - ix) / (nextIx - ix);
    }
    return points[size - 1]->y();
}
}  // namespace

void AVTPlotWidget::paintEvent(QPaintEvent *e)
{
    Q_UNUSED(e)

    QPainter p;

    p.begin(this);
    p.setRenderHint(QPainter::Antialiasing, antialiasing());
    p.fillRect(rect(), backgroundColor());
    p.translate(leftPadding(), topPadding());

    setPixRect();
    p.setClipRect(pixRect());
    p.setClipping(true);

    int pW = pixRect().width();
    int pH = pixRect().height();

    QColor SkyColor(0, 100, 200);
    /*
    if (Options::darkAppColors())
        SkyColor = QColor(200, 0, 0); // use something red, visible through a red filter
        */

    // Draw gradient representing lunar interference in the sky
    if (MoonIllum > 0.01) // do this only if Moon illumination is reasonable so it's important
    {
        double moonrise = pW * (0.5 + MoonRise);
        double moonset  = pW * (MoonSet - 0.5);
        if (moonset < 0)
            moonset += pW;
        if (moonrise > pW)
            moonrise -= pW;
        moonrise = convertCoords(moonrise);
        moonset = convertCoords(moonset);

        const int mooncolor = int(10 + MoonIllum * 130);
        const QColor MoonColor(mooncolor, mooncolor, mooncolor);
        int fadewidth =
            pW *
            0.01; // pW * fraction of day to fade the moon brightness over (0.01 corresponds to roughly 15 minutes, 0.007 to 10 minutes), both before and after actual set.

        drawMoon(p, int(moonrise), int(moonset), fadewidth, MoonColor, pW, pH);

    }
    //draw daytime sky if the Sun rises for the current date/location
    if (SunMaxAlt > -18.0)
    {
        // Initially compute centered on midnight, so modulate dawn/dusk by 0.5
        // Then convert to general coordinates.
        int rise = convertCoords(pW * (0.5 + SunRise));
        int set  = convertCoords(pW * (SunSet - 0.5));
        int dawn = convertCoords(pW * (0.5 + Dawn));
        double dusk = int(pW * (Dusk - 0.5));
        if (dusk < 0) dusk = pW + dusk;
        dusk = convertCoords(dusk);

        if (SunMinAlt > 0.0)
        {
            // The sun never set and the sky is always blue
            p.fillRect(rect(), SkyColor);
        }
        else drawSun(p, rise, set, SunMinAlt, SunMaxAlt, dawn, int(dusk), Dawn < 0.0, SkyColor, pW, pH);
    }

    //draw ground
    if (altitudeAxisMin < 0)
    {
        const int groundYValue = pH + altitudeAxisMin * pH / (altitudeAxisMax - altitudeAxisMin);
        p.fillRect(0, groundYValue, pW, groundYValue,
                   KStarsData::Instance()->colorScheme()->colorNamed(
                       "HorzColor")); // asimha changed to use color from scheme. Formerly was QColor( "#002200" )
    }

    foreach (KPlotObject *po, plotObjects())
    {
        po->draw(&p, this);
    }

    p.setClipping(false);
    drawAxes(&p);

    //Add vertical line indicating "now"
    QFont smallFont = p.font();
    smallFont.setPointSize(smallFont.pointSize()); // wat?
    if (geo)
    {
        QTime t = geo->UTtoLT(KStarsDateTime::currentDateTimeUtc())
                  .time(); // convert the current system clock time to the TZ corresponding to geo
        double x = 12.0 + t.hour() + t.minute() / 60.0 + t.second() / 3600.0;
        while (x > 24.0)
            x -= 24.0;
        double ix = x * pW / 24.0; //convert to screen pixel coords
        ix = convertCoords(ix);
        p.setPen(QPen(QBrush("white"), 2.0, Qt::DotLine));
        p.drawLine(int(ix), 0, ix, pH);

        //Label this vertical line with the current time
        p.save();
        p.setFont(smallFont);
        p.translate(int(ix) + 10, pH - 20);
        p.rotate(-90);
        p.drawText(
            0, 0,
            QLocale().toString(t, QLocale::ShortFormat)); // short format necessary to avoid false time-zone labeling
        p.restore();
    }

    //Draw crosshairs at clicked position
    if (MousePoint.x() > 0)
    {
        p.setPen(QPen(QBrush("gold"), 1.0, Qt::SolidLine));
        p.drawLine(QLineF(MousePoint.x() + 0.5, 0.5, MousePoint.x() + 0.5, pixRect().height() - 0.5));

        //Label each crosshair line (time and altitude)
        p.setFont(smallFont);

        double h = (MousePoint.x() * plotDuration) / pW - (12.0 - noonOffset);
        double a = 0;
        if (plotObjects().size() > 0)
            a = findYValue(plotObjects()[0], h);
        p.drawText(15, 15, QString::number(a, 'f', 1) + QChar(176));

        if (h < 0.0)
            h += 24.0;
        QTime t = QTime(int(h), int(60. * (h - int(h))));
        p.save();
        p.translate(MousePoint.x() + 10, pH - 20);
        p.rotate(-90);
        p.drawText(
            0, 0,
            QLocale().toString(t, QLocale::ShortFormat)); // short format necessary to avoid false time-zone labeling
        p.restore();
    }

    p.end();
}

void AVTPlotWidget::setDawnDuskTimes(double da, double du)
{
    Dawn = da;
    Dusk = du;
    update(); // fixme: should we always be calling update? It's probably cheap enough that we can.
}

void AVTPlotWidget::setMinMaxSunAlt(double min, double max)
{
    SunMinAlt = min;
    SunMaxAlt = max;
    update();
}

void AVTPlotWidget::setSunRiseSetTimes(double sr, double ss)
{
    SunRise = sr;
    SunSet  = ss;
    update();
}

void AVTPlotWidget::setMoonRiseSetTimes(double mr, double ms)
{
    MoonRise = mr;
    MoonSet  = ms;
    update();
}

void AVTPlotWidget::setMoonIllum(double mi)
{
    MoonIllum = mi;
    update();
}

void AVTPlotWidget::setPlotExtent(double offset, double duration)
{
    noonOffset = offset;
    plotDuration = duration;
}

void AVTPlotWidget::plot(const GeoLocation *geo, KSAlmanac *ksal,
                         const QVector<double> &times, const QVector<double> &alts, bool overlay)
{
    KPlotObject *po = new KPlotObject(Qt::white, KPlotObject::Lines, 2);
    if (overlay)
    {
        QPen pen;
        pen.setWidth(5);
        pen.setColor(Qt::green);
        po->setLinePen(pen);
    }
    else
    {
        setLimits(times[0], times.last(), altitudeAxisMin, altitudeAxisMax);
        setSecondaryLimits(times[0], times.last(), altitudeAxisMin, altitudeAxisMax);
        axis(KPlotWidget::BottomAxis)->setTickLabelFormat('t');
        axis(KPlotWidget::TopAxis)->setTickLabelFormat('t');
        axis(KPlotWidget::TopAxis)->setTickLabelsShown(true);
        setGeoLocation(geo);

        setSunRiseSetTimes(ksal->getSunRise(), ksal->getSunSet());
        setDawnDuskTimes(ksal->getDawnAstronomicalTwilight(), ksal->getDuskAstronomicalTwilight());
        setMinMaxSunAlt(ksal->getSunMinAlt(), ksal->getSunMaxAlt());
        setMoonRiseSetTimes(ksal->getMoonRise(), ksal->getMoonSet());
        setMoonIllum(ksal->getMoonIllum());

        const double noonOffset = times[0] - -12;
        const double plotDuration = times.last() - times[0];
        setPlotExtent(noonOffset, plotDuration);
        removeAllPlotObjects();
    }

    for (int i = 0; i < times.size(); ++i)
        po->addPoint(times[i], alts[i]);
    addPlotObject(po);

    update();
}

void AVTPlotWidget::setAltitudeAxis(double min, double max)
{
    if (min < max)
    {
        altitudeAxisMin = min;
        altitudeAxisMax = max;
    }
}

