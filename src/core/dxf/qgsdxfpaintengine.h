/***************************************************************************
                         qgsdxpaintengine.h
                         ------------------
    begin                : November 2013
    copyright            : (C) 2013 by Marco Hugentobler
    email                : marco at sourcepole dot ch
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#ifndef QGSDXFPAINTENGINE_H
#define QGSDXFPAINTENGINE_H

#include <QPaintEngine>
#include "qgsgeometryfactory.h"
#include "qgsabstractgeometryv2.h"

class QgsDxfExport;
class QgsDxfPaintDevice;


/** \ingroup core
 * \class QgsDxfPaintEngine
 * \note not available in Python bindings
*/

class CORE_EXPORT QgsDxfPaintEngine: public QPaintEngine
{
  public:
    QgsDxfPaintEngine( const QgsDxfPaintDevice* dxfDevice, QgsDxfExport* dxf );
    ~QgsDxfPaintEngine();

    bool begin( QPaintDevice* pdev ) override;
    bool end() override;
    QPaintEngine::Type type() const override;
    void updateState( const QPaintEngineState& state ) override;

    void drawPixmap( const QRectF& r, const QPixmap& pm, const QRectF& sr ) override;

    void drawPolygon( const QPointF * points, int pointCount, PolygonDrawMode mode ) override;
    void drawPath( const QPainterPath& path ) override;
    void drawLines( const QLineF* lines, int lineCount ) override;

    void setLayer( const QString& layer ) { mLayer = layer; }
    QString layer() const { return mLayer; }

    void setShift( QPointF shift ) { mShift = shift; }

  private:
    const QgsDxfPaintDevice* mPaintDevice;
    QgsDxfExport* mDxf;

    //painter state information
    QTransform mTransform;
    QPen mPen;
    QBrush mBrush;
    /** Opacity*/
    double mOpacity;
    QString mLayer;
    QPointF mShift;
    QgsRingSequenceV2 mPolygon;
    QPolygonF mCurrentPolygon;
    QList<QPointF> mCurrentCurve;

    QgsPointV2 toDxfCoordinates( QPointF pt ) const;
    double currentWidth() const;

    void moveTo( double dx, double dy );
    void lineTo( double dx, double dy );
    void curveTo( double dx, double dy );
    void endPolygon();
    void endCurve();

    void setRing( QgsPointSequenceV2 &polyline, const QPointF * points, int pointCount );

    //utils for bezier curve calculation
    static QPointF bezierPoint( const QList<QPointF>& controlPolygon, double t );
    static double bernsteinPoly( int n, int i, double t );
    static int lower( int n, int i );
    static double power( double a, int b );
    static int faculty( int n );

    /** Returns current pen color*/
    QColor penColor() const;
    /** Returns current brush color*/
    QColor brushColor() const;
};

#endif // QGSDXFPAINTENGINE_H
