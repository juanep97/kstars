/*
    SPDX-FileCopyrightText: 2021 Hy Murveit <hy@murveit.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "polaralign.h"
#include "poleaxis.h"
#include "rotations.h"

#include <cmath>

#include "fitsviewer/fitsdata.h"
#include "kstarsdata.h"
#include "skypoint.h"
#include <ekos_align_debug.h>

/******************************************************************
PolarAlign is a class that supports polar alignment by determining the
mount's axis of rotation when given 3 solved images taken with RA mount
rotations between the images.

addPoint(image) is called by the polar alignment UI after it takes and
solves each of its three images. The solutions are store in SkyPoints (see below)
and are processed so that the sky positions correspond to "what's in the sky
now" and "at this geographic localtion".

Addpoint() samples the location of a particular pixel in its image.
When the 3 points are sampled, they should not be taken
from the center of the image, as HA rotations may not move that point
if the telescope and mount are well aligned. Thus, the points are sampled
from the edge of the image.

After all 3 images are sampled, findAxis() is called, which solves for the mount's
axis of rotation. It then transforms poleAxis' result into azimuth and altitude
offsets from the pole.

After the mount's current RA axis is determined, the user then attempts to correct/improve
it to match the Earth's real polar axes. Ekos has two techniques to do that. In both
Ekos takes a series of "refresh images".  The user looks at the images and their
associated analyses and adjusts the mount's altitude and azimuth knobs.

In the first scheme, the user identifies a refrence star on the image. Ekos draws a triangle
over the image, and user attempts to "move the star" along two sides of that triangle.

In the 2nd scheme, the system plate-solves the refresh images, telling the user which direction
and how much to adjust the knobs.

findCorrectedPixel() supports the "move the star" refresh scheme.
It is given an x,y position on an image and the offsets
generated by findAxis(). It computes a "corrected position" for that input
x,y point such that if a user adjusted the GEM mount's altitude and azimuth
knobs to move a star centered in the image's original x,y position to the corrected
position in the image, the mount's axis of rotation should then coincide with the pole.

processRefreshCoords() supports the plate-solving refresh scheme.
It is given the center coordinates of a refresh image. It remembers the originally
calculated mount axis, and the position of the 3rd measurement image. It computes how
much the user has already adjusted the azimuth and altitude knobs from the difference
in pointing between the new refresh image's center coordinates and that of the 3rd measurement
image. It infers what the mounts new RA axis must be (based on that adjustment) and returns
the new polar alignment error.
******************************************************************/

using Rotations::V3;

PolarAlign::PolarAlign(const GeoLocation *geo)
{
    if (geo == nullptr && KStarsData::Instance() != nullptr)
        geoLocation = KStarsData::Instance()->geo();
    else
        geoLocation = geo;
}

bool PolarAlign::northernHemisphere() const
{
    if ((geoLocation == nullptr) || (geoLocation->lat() == nullptr))
        return true;
    return geoLocation->lat()->Degrees() > 0;
}

void PolarAlign::reset()
{
    points.clear();
    times.clear();
}

// Gets the pixel's j2000 RA&DEC coordinates, converts to JNow,  adjust to
// the local time, and sets up the azimuth and altitude coordinates.
bool PolarAlign::prepareAzAlt(const QSharedPointer<FITSData> &image, const QPointF &pixel, SkyPoint *point) const
{
    // WCS must be set up for this image.
    SkyPoint coords;
    if (image && image->pixelToWCS(pixel, coords))
    {
        coords.apparentCoord(static_cast<long double>(J2000), image->getDateTime().djd());
        *point = SkyPoint::timeTransformed(&coords, image->getDateTime(), geoLocation, 0);
        return true;
    }
    return false;
}

bool PolarAlign::addPoint(const QSharedPointer<FITSData> &image)
{
    SkyPoint coords;
    auto time = image->getDateTime();
    // Use the HA and DEC from the center of the image.
    if (!prepareAzAlt(image, QPointF(image->width() / 2, image->height() / 2), &coords))
        return false;

    QString debugString = QString("PAA: addPoint ra0 %1 dec0 %2 ra %3 dec %4 az %5 alt %6")
                          .arg(coords.ra0().Degrees()).arg(coords.dec0().Degrees())
                          .arg(coords.ra().Degrees()).arg(coords.dec().Degrees())
                          .arg(coords.az().Degrees()).arg(coords.alt().Degrees());
    qCInfo(KSTARS_EKOS_ALIGN) << debugString;
    if (points.size() > 2)
        return false;
    points.push_back(coords);
    times.push_back(time);

    return true;
}

namespace
{

// Returns the distance, in absolute-value degrees, of taking point "from",
// rotating it around the Y axis by yAngle, then rotating around the Z axis
// by zAngle and comparing that with "goal".
double getResidual(const V3 &from, double yAngle, double zAngle, const V3 &goal)
{
    V3 point1 = Rotations::rotateAroundY(from, yAngle);
    V3 point2 = Rotations::rotateAroundZ(point1, zAngle);
    return fabs(getAngle(point2, goal));
}

// Finds the best rotations to change from pointing to 'from' to pointing to 'goal'.
// It tries 'all' possible pairs of x and y rotations (sampled by increment).
// Note that you can't simply find the best Z rotation, and the go from there to find the best Y.
// The space is non-linear, and that would often lead to poor solutions.
double getBestRotation(const V3 &from, const V3 &goal,
                       double zStart, double yStart,
                       double *bestAngleZ, double *bestAngleY,
                       double range, double increment)
{

    *bestAngleZ = 0;
    *bestAngleY = 0;
    double minDist = 1e8;
    range = fabs(range);
    for (double thetaY = yStart - range; thetaY <= yStart + range; thetaY += increment)
    {
        for (double thetaZ = zStart - range; thetaZ <= zStart + range; thetaZ += increment)
        {
            double dist = getResidual(from, thetaY, thetaZ, goal);
            if (dist < minDist)
            {
                minDist = dist;
                *bestAngleY = thetaY;
                *bestAngleZ = thetaZ;
            }
        }
    }
    return minDist;
}

// Computes the rotations in Y (altitude) and Z (azimuth) that brings 'from' closest to 'goal'.
// Returns the residual (error angle between where these rotations lead and "goal".
double getRotationAngles(const V3 &from, const V3 &goal, double *zAngle, double *yAngle)
{
    // All in degrees.
    constexpr double pass1Resolution = 1.0 / 60.0;
    constexpr double pass2Resolution = 5 / 3600.0;
    constexpr double pass2Range = 4.0 / 60.0;

    // Compute the rotation using a great circle. This somewhat constrains our search below.
    const double rotationAngle = getAngle(from, goal); // degrees
    const double pass1Range = std::max(1.0, std::min(10.0, 2.5 * fabs(rotationAngle)));

    // Grid search across all y,z angle possibilities, sampling by 2 arc-minutes.
    const double pass1Residual = getBestRotation(from, goal, 0, 0, zAngle, yAngle, pass1Range, pass1Resolution);
    Q_UNUSED(pass1Residual);

    // Refine the search around the best solution so far
    return getBestRotation(from, goal, *zAngle, *yAngle, zAngle, yAngle, pass2Range, pass2Resolution);
}

}  // namespace

// Compute the polar-alignment azimuth and altitude error by comparing the new image's coordinates
// with the coordinates from the 3rd measurement image. Use the difference to infer a rotation angle,
// and rotate the originally computed polar-alignment axis by that angle to find the new axis
// around which RA now rotates.
bool PolarAlign::processRefreshCoords(const SkyPoint &coords, const KStarsDateTime &time,
                                      double *azError, double *altError,
                                      double *azAdjustment, double *altAdjustment) const
{
    // Get the az and alt from this new measurement (coords), and from that derive its x,y,z coordinates.
    auto c = coords;  // apparentCoord modifies its input. Use the temp variable c to keep coords const.
    c.apparentCoord(static_cast<long double>(J2000), time.djd());
    SkyPoint point = SkyPoint::timeTransformed(&c, time, geoLocation, 0);
    const double az = point.az().Degrees(), alt = point.alt().Degrees();
    const V3 newPoint = Rotations::azAlt2xyz(QPointF(az, alt));

    // Get the x,y,z coordinates of the original position (from the 3rd polar-align image).
    // We can't simply use the az/alt already computed for point3 because the mount is tracking,
    // and thus, even if the user made no adjustments, but simply took an image a little while
    // later at the same RA/DEC coordinates, the Az/Alt would have changed and we'd believe there
    // was a user rotation due to changes in the Az/Alt knobs. Instead we must convert point3's az/alt
    // values to what they would be if that image had been taken now, still pointing at point3's ra/dec.
    // The key is to rotate the original point around the original RA axis by the rotation given by the time
    // difference multiplied by the sidereal rate.

    // Figure out what the az/alt would be if the user hadn't modified the knobs.
    // That is, just rotate the 3rd measurement point around the mount's original RA axis.
    // Time since third point in seconds
    const double p3secs = times[2].secsTo(time);
    // Angle corresponding to that interval assuming the sidereal rate.
    const double p3Angle = (-15.041067 * p3secs) / 3600.0;  // degrees

    // Get the xyz coordinates of the original 3rd point.
    const V3 p3OrigPoint = Rotations::azAlt2xyz(QPointF(points[2].az().Degrees(), points[2].alt().Degrees()));
    // Get the unit vector corresponding the original RA axis
    const V3 origAxisPt = Rotations::azAlt2xyz(QPointF(azimuthCenter, altitudeCenter));
    // Rotate the original 3rd point around that axis, simulating the mount's tracking movements.
    const V3 point3 = Rotations::rotateAroundAxis(p3OrigPoint, origAxisPt, p3Angle);

    // Find the adjustment the user must have made by examining the change from point3 to newPoint
    // (i.e. the rotation caused by the user adjusting the azimuth and altitude knobs).
    // We assume that this was a rotation around a level mount's y axis and z axis.
    double zAdjustment, yAdjustment;
    double residual = getRotationAngles(point3, newPoint, &zAdjustment, &yAdjustment);
    if (residual > 0.5)
    {
        qCInfo(KSTARS_EKOS_ALIGN) << QString("PAA refresh: failed to estimate rotation angle (residual %1'").arg(residual * 60);
        return false;
    }
    qCInfo(KSTARS_EKOS_ALIGN) << QString("PAA refresh: Estimated current adjustment: Az %1' Alt %2' residual %3a-s")
                              .arg(zAdjustment * 60, 0, 'f', 1).arg(yAdjustment * 60, 0, 'f', 1).arg(residual * 3600, 0, 'f', 0);

    // Return the estimated adjustments (used by testing).
    if (altAdjustment != nullptr) *altAdjustment = yAdjustment;
    if (azAdjustment != nullptr) *azAdjustment = zAdjustment;

    // Rotate the original RA axis position by the above adjustments.
    const V3 origAxisPoint = Rotations::azAlt2xyz(QPointF(azimuthCenter, altitudeCenter));
    const V3 tempPoint = Rotations::rotateAroundY(origAxisPoint, yAdjustment);
    const V3 newAxisPoint = Rotations::rotateAroundZ(tempPoint, zAdjustment);

    // Convert the rotated axis point back to an az/alt coordinate, representing the new RA axis.
    const QPointF newAxisAzAlt = Rotations::xyz2azAlt(newAxisPoint);
    const double newAxisAz = newAxisAzAlt.x();
    const double newAxisAlt = newAxisAzAlt.y();

    // Compute the polar alignment error for the new RA axis.
    const double latitudeDegrees = geoLocation->lat()->Degrees();
    *altError = northernHemisphere() ? newAxisAlt - latitudeDegrees : newAxisAlt + latitudeDegrees;
    *azError = northernHemisphere() ? newAxisAz : newAxisAz + 180.0;
    while (*azError > 180.0)
        *azError -= 360;

    QString infoString =
        QString("PAA refresh: ra0 %1 dec0 %2 Az/Alt: %3 %4 AXIS: %5 %6 --> %7 %8 ERR: %9' alt %10'")
        .arg(coords.ra0().Degrees(), 0, 'f', 3).arg(coords.dec0().Degrees(), 0, 'f', 3)
        .arg(az, 0, 'f', 3).arg(alt, 0, 'f', 3)
        .arg(azimuthCenter, 0, 'f', 3).arg(altitudeCenter, 0, 'f', 3)
        .arg(newAxisAz, 0, 'f', 3).arg(newAxisAlt, 0, 'f', 3)
        .arg(*azError * 60, 0, 'f', 1).arg(*altError * 60, 0, 'f', 1);
    qCInfo(KSTARS_EKOS_ALIGN) << infoString;

    return true;
}

// Given the telescope's current RA axis, and the its current pointing position,
// compute the coordinates where it should point such that its RA axis will be at the pole.
bool PolarAlign::refreshSolution(SkyPoint *solution, SkyPoint *altOnlySolution) const
{
    if (points.size() != 3)
        return false;

    double azError, altError;
    calculateAzAltError(&azError, &altError);

    // The Y rotation to correct polar alignment is -altitude error, and the Z correction is -azimuth error.
    // Rotate the 3rd-image center coordinate by the above angles.
    // This is the position the telescope needs to point to (if it is taken there
    // by adjusting alt and az knobs) such that the new RA rotation axis is aligned with the pole.
    const V3 point3 = Rotations::azAlt2xyz(QPointF(points[2].az().Degrees(), points[2].alt().Degrees()));
    const V3 altSolutionPoint = Rotations::rotateAroundY(point3, altError);
    const V3 solutionPoint = Rotations::rotateAroundZ(altSolutionPoint, azError);

    // Convert the solution xyz points back to az/alt and ra/dec.
    const QPointF solutionAzAlt = Rotations::xyz2azAlt(solutionPoint);
    solution->setAz(solutionAzAlt.x());
    solution->setAlt(solutionAzAlt.y());
    auto lst = geoLocation->GSTtoLST(times[2].gst());
    solution->HorizontalToEquatorial(&lst, geoLocation->lat());

    // Not sure if this is needed
    solution->setRA0(solution->ra());
    solution->setDec0(solution->dec());

    // Move the solution back to J2000
    KSNumbers num(times[2].djd());
    *solution = solution->deprecess(&num);
    solution->setRA0(solution->ra());
    solution->setDec0(solution->dec());

    // Ditto for alt-only solution
    const QPointF altOnlySolutionAzAlt = Rotations::xyz2azAlt(altSolutionPoint);
    altOnlySolution->setAz(altOnlySolutionAzAlt.x());
    altOnlySolution->setAlt(altOnlySolutionAzAlt.y());
    auto altOnlyLst = geoLocation->GSTtoLST(times[2].gst());
    altOnlySolution->HorizontalToEquatorial(&altOnlyLst, geoLocation->lat());
    altOnlySolution->setRA0(altOnlySolution->ra());
    altOnlySolution->setDec0(altOnlySolution->dec());
    KSNumbers altOnlyNum(times[2].djd());
    *altOnlySolution = altOnlySolution->deprecess(&altOnlyNum);
    altOnlySolution->setRA0(altOnlySolution->ra());
    altOnlySolution->setDec0(altOnlySolution->dec());

    return true;
}

bool PolarAlign::findAxis()
{
    if (points.size() != 3)
        return false;

    // We have 3 points, get their xyz positions.
    V3 p1(Rotations::azAlt2xyz(QPointF(points[0].az().Degrees(), points[0].alt().Degrees())));
    V3 p2(Rotations::azAlt2xyz(QPointF(points[1].az().Degrees(), points[1].alt().Degrees())));
    V3 p3(Rotations::azAlt2xyz(QPointF(points[2].az().Degrees(), points[2].alt().Degrees())));
    V3 axis = Rotations::getAxis(p1, p2, p3);

    if (axis.length() < 0.9)
    {
        // It failed to normalize the vector, something's wrong.
        qCInfo(KSTARS_EKOS_ALIGN) << "Normal vector too short. findAxis failed.";
        return false;
    }

    // Need to make sure we're pointing to the right pole.
    if ((northernHemisphere() && (axis.x() < 0)) || (!northernHemisphere() && axis.x() > 0))
    {
        axis = V3(-axis.x(), -axis.y(), -axis.z());
    }

    QPointF azAlt = Rotations::xyz2azAlt(axis);
    azimuthCenter = azAlt.x();
    altitudeCenter = azAlt.y();

    return true;
}

void PolarAlign::getAxis(double *azAxis, double *altAxis) const
{
    *azAxis = azimuthCenter;
    *altAxis = altitudeCenter;
}

// Find the pixel in image corresponding to the desired azimuth & altitude.
bool PolarAlign::findAzAlt(const QSharedPointer<FITSData> &image, double azimuth, double altitude, QPointF *pixel) const
{
    SkyPoint spt;
    spt.setAz(azimuth);
    spt.setAlt(altitude);
    dms LST = geoLocation->GSTtoLST(image->getDateTime().gst());
    spt.HorizontalToEquatorial(&LST, geoLocation->lat());
    SkyPoint j2000Coord = spt.catalogueCoord(image->getDateTime().djd());
    QPointF imagePoint;
    if (!image->wcsToPixel(j2000Coord, *pixel, imagePoint))
    {
        QString debugString =
            QString("PolarAlign: Couldn't get pixel from WCS for az %1 alt %2 with j2000 RA %3 DEC %4")
            .arg(QString::number(azimuth), QString::number(altitude), j2000Coord.ra0().toHMSString(), j2000Coord.dec0().toDMSString());
        qCInfo(KSTARS_EKOS_ALIGN) << debugString;
        return false;
    }
    return true;
}

// Calculate the mount's azimuth and altitude error given the known geographic location
// and the azimuth center and altitude center computed in findAxis().
void PolarAlign::calculateAzAltError(double *azError, double *altError) const
{
    const double latitudeDegrees = geoLocation->lat()->Degrees();
    *altError = northernHemisphere() ?
                altitudeCenter - latitudeDegrees : altitudeCenter + latitudeDegrees;
    *azError = northernHemisphere() ? azimuthCenter : azimuthCenter + 180.0;
    while (*azError > 180.0)
        *azError -= 360;
}

void PolarAlign::setMaxPixelSearchRange(double degrees)
{
    // Suggestion for how far pixelError() below searches.
    // Don't allow the search to be modified too much.
    const double d = fabs(degrees);
    if (d < 2)
        maxPixelSearchRange = 2.0;
    else if (d > 10)
        maxPixelSearchRange = 10.0;
    else
        maxPixelSearchRange = d;
}

// Given the currently estimated RA axis polar alignment error, and given a start pixel,
// find the polar-alignment error if the user moves a star (from his point of view)
// from that pixel to pixel2.
//
// FindCorrectedPixel() determines where the user should move the star to fully correct
// the alignment error. However, while the user is doing that, he/she may be at an intermediate
// point (pixel2) and we want to feed back to the user what the "current" polar-alignment error is.
// This searches using findCorrectedPixel() to
// find the RA axis error which would be fixed by the user moving pixel to pixel2. The input
// thus should be pixel = "current star position", and pixel2 = "solution star position"
// from the original call to findCorrectedPixel. This calls findCorrectedPixel several hundred times
// but is not too costly (about .1s on a RPi4).  One could write a method that more directly estimates
// the error given the current position, but it might not be applicable to our use-case as
// we are constrained to move along paths detemined by a user adjusting an altitude knob and then
// an azimuth adjustment. These corrections are likely not the most direct path to solve the axis error.
bool PolarAlign::pixelError(const QSharedPointer<FITSData> &image, const QPointF &pixel, const QPointF &pixel2,
                            double *azError, double *altError)
{
    double azOffset, altOffset;
    calculateAzAltError(&azOffset, &altOffset);

    QPointF pix;
    double azE = 0, altE = 0;

    pixelError(image, pixel, pixel2,
               -maxPixelSearchRange, maxPixelSearchRange, 0.2,
               -maxPixelSearchRange, maxPixelSearchRange, 0.2, &azE, &altE, &pix);
    pixelError(image, pixel, pixel2, azE - .2, azE + .2, 0.02,
               altE - .2, altE + .2, 0.02, &azE, &altE, &pix);
    pixelError(image, pixel, pixel2, azE - .02, azE + .02, 0.002,
               altE - .02, altE + .02, 0.002, &azE, &altE, &pix);

    const double pixDist = hypot(pix.x() - pixel2.x(), pix.y() - pixel2.y());
    if (pixDist > 10)
        return false;

    *azError = azE;
    *altError = altE;
    return true;
}

void PolarAlign::pixelError(const QSharedPointer<FITSData> &image, const QPointF &pixel, const QPointF &pixel2,
                            double minAz, double maxAz, double azInc,
                            double minAlt, double maxAlt, double altInc,
                            double *azError, double *altError, QPointF *actualPixel)
{
    double minDistSq = 1e9;
    for (double eAz = minAz; eAz < maxAz; eAz += azInc)
    {
        for (double eAlt = minAlt; eAlt < maxAlt; eAlt += altInc)
        {
            QPointF pix;
            if (findCorrectedPixel(image, pixel, &pix, eAz, eAlt))
            {
                // compare the distance to the pixel
                double distSq = ((pix.x() - pixel2.x()) * (pix.x() - pixel2.x()) +
                                 (pix.y() - pixel2.y()) * (pix.y() - pixel2.y()));
                if (distSq < minDistSq)
                {
                    minDistSq = distSq;
                    *actualPixel = pix;
                    *azError = eAz;
                    *altError = eAlt;
                }
            }
        }
    }
}

// Given a pixel, find its RA/DEC, then its alt/az, and then solve for another pixel
// where, if the star in pixel is moved to that star in the user's image (by adjusting alt and az controls)
// the polar alignment error would be 0.
bool PolarAlign::findCorrectedPixel(const QSharedPointer<FITSData> &image, const QPointF &pixel, QPointF *corrected,
                                    bool altOnly)
{
    double azOffset, altOffset;
    calculateAzAltError(&azOffset, &altOffset);
    if (altOnly)
        azOffset = 0.0;
    return findCorrectedPixel(image, pixel, corrected, azOffset, altOffset);
}

// Given a pixel, find its RA/DEC, then its alt/az, and then solve for another pixel
// where, if the star in pixel is moved to that star in the user's image (by adjusting alt and az controls)
// the polar alignment error would be 0. We use the fact that we can only move by adjusting and altitude
// knob, then an azimuth knob--i.e. we likely don't traverse a great circle.
bool PolarAlign::findCorrectedPixel(const QSharedPointer<FITSData> &image, const QPointF &pixel, QPointF *corrected,
                                    double azOffset,
                                    double altOffset)
{
    // 1. Find the az/alt for the x,y point on the image.
    SkyPoint p;
    if (!prepareAzAlt(image, pixel, &p))
        return false;
    double pixelAz = p.az().Degrees(), pixelAlt = p.alt().Degrees();

    // 2. Apply the az/alt offsets.
    // We know that the pole's az and alt offsets are effectively rotations
    // of a sphere. The offsets that apply to correct different points depend
    // on where (in the sphere) those points are. Points close to the pole can probably
    // just add the pole's offsets. This calculation is a bit more precise, and is
    // necessary if the points are not near the pole.
    double altRotation = northernHemisphere() ? altOffset : -altOffset;
    QPointF rotated = Rotations::rotateRaAxis(QPointF(pixelAz, pixelAlt), QPointF(azOffset, altRotation));

    // 3. Find a pixel with those alt/az values.
    if (!findAzAlt(image, rotated.x(), rotated.y(), corrected))
        return false;

    return true;
}
