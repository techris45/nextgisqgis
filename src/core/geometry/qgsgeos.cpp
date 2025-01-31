/***************************************************************************
                        qgsgeos.cpp
  -------------------------------------------------------------------
Date                 : 22 Sept 2014
Copyright            : (C) 2014 by Marco Hugentobler
email                : marco.hugentobler at sourcepole dot com
 ***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include "qgsgeos.h"
#include "qgsabstractgeometryv2.h"
#include "qgsgeometrycollectionv2.h"
#include "qgsgeometryfactory.h"
#include "qgslinestringv2.h"
#include "qgsmessagelog.h"
#include "qgsmulticurvev2.h"
#include "qgsmultilinestringv2.h"
#include "qgsmultipointv2.h"
#include "qgsmultipolygonv2.h"
#include "qgslogger.h"
#include "qgspolygonv2.h"
#include <limits>
#include <cstdio>
#include <QtCore/qmath.h>

#define DEFAULT_QUADRANT_SEGMENTS 8

#define CATCH_GEOS(r) \
  catch (GEOSException &e) \
  { \
    QgsMessageLog::logMessage( QObject::tr( "Exception: %1" ).arg( e.what() ), QObject::tr("GEOS") ); \
    return r; \
  }

#define CATCH_GEOS_WITH_ERRMSG(r) \
  catch (GEOSException &e) \
  { \
    QgsMessageLog::logMessage( QObject::tr( "Exception: %1" ).arg( e.what() ), QObject::tr("GEOS") ); \
    if ( errorMsg ) \
    { \
      *errorMsg = e.what(); \
    } \
    return r; \
  }

/// @cond PRIVATE

static void throwGEOSException( const char *fmt, ... )
{
  va_list ap;
  char buffer[1024];

  va_start( ap, fmt );
  vsnprintf( buffer, sizeof buffer, fmt, ap );
  va_end( ap );

  QgsDebugMsg( QString( "GEOS exception: %1" ).arg( buffer ) );

  throw GEOSException( QString::fromUtf8( buffer ) );
}

static void printGEOSNotice( const char *fmt, ... )
{
#if defined(QGISDEBUG)
  va_list ap;
  char buffer[1024];

  va_start( ap, fmt );
  vsnprintf( buffer, sizeof buffer, fmt, ap );
  va_end( ap );

  QgsDebugMsg( QString( "GEOS notice: %1" ).arg( QString::fromUtf8( buffer ) ) );
#else
  Q_UNUSED( fmt );
#endif
}

class GEOSInit
{
  public:
    GEOSContextHandle_t ctxt;

    GEOSInit()
    {
      ctxt = initGEOS_r( printGEOSNotice, throwGEOSException );
    }

    ~GEOSInit()
    {
      finishGEOS_r( ctxt );
    }

  private:

    GEOSInit( const GEOSInit& rh );
    GEOSInit& operator=( const GEOSInit& rh );
};

static GEOSInit geosinit;

///@endcond


/** \ingroup core
 * @brief Scoped GEOS pointer
 * @note not available in Python bindings
 */
class GEOSGeomScopedPtr
{
  public:
    explicit GEOSGeomScopedPtr( GEOSGeometry* geom = nullptr ) : mGeom( geom ) {}
    ~GEOSGeomScopedPtr() { GEOSGeom_destroy_r( geosinit.ctxt, mGeom ); }
    GEOSGeometry* get() const { return mGeom; }
    operator bool() const { return nullptr != mGeom; }
    void reset( GEOSGeometry* geom )
    {
      GEOSGeom_destroy_r( geosinit.ctxt, mGeom );
      mGeom = geom;
    }

  private:
    GEOSGeometry* mGeom;

  private:
    GEOSGeomScopedPtr( const GEOSGeomScopedPtr& rh );
    GEOSGeomScopedPtr& operator=( const GEOSGeomScopedPtr& rh );
};

QgsGeos::QgsGeos( const QgsAbstractGeometryV2* geometry, double precision )
    : QgsGeometryEngine( geometry )
    , mGeos( nullptr )
    , mGeosPrepared( nullptr )
    , mPrecision( precision )
{
  cacheGeos();
}

QgsGeos::~QgsGeos()
{
  GEOSGeom_destroy_r( geosinit.ctxt, mGeos );
  mGeos = nullptr;
  GEOSPreparedGeom_destroy_r( geosinit.ctxt, mGeosPrepared );
  mGeosPrepared = nullptr;
}

void QgsGeos::geometryChanged()
{
  GEOSGeom_destroy_r( geosinit.ctxt, mGeos );
  mGeos = nullptr;
  GEOSPreparedGeom_destroy_r( geosinit.ctxt, mGeosPrepared );
  mGeosPrepared = nullptr;
  cacheGeos();
}

void QgsGeos::prepareGeometry()
{
  GEOSPreparedGeom_destroy_r( geosinit.ctxt, mGeosPrepared );
  mGeosPrepared = nullptr;
  if ( mGeos )
  {
    mGeosPrepared = GEOSPrepare_r( geosinit.ctxt, mGeos );
  }
}

void QgsGeos::cacheGeos() const
{
  if ( !mGeometry || mGeos )
  {
    return;
  }

  mGeos = asGeos( mGeometry, mPrecision );
}

QgsAbstractGeometryV2* QgsGeos::intersection( const QgsAbstractGeometryV2& geom, QString* errorMsg ) const
{
  return overlay( geom, INTERSECTION, errorMsg );
}

QgsAbstractGeometryV2* QgsGeos::difference( const QgsAbstractGeometryV2& geom, QString* errorMsg ) const
{
  return overlay( geom, DIFFERENCE, errorMsg );
}

QgsAbstractGeometryV2* QgsGeos::combine( const QgsAbstractGeometryV2& geom, QString* errorMsg ) const
{
  return overlay( geom, UNION, errorMsg );
}

QgsAbstractGeometryV2* QgsGeos::combine( const QList<QgsAbstractGeometryV2*>& geomList, QString* errorMsg ) const
{

  QVector< GEOSGeometry* > geosGeometries;
  geosGeometries.resize( geomList.size() );
  for ( int i = 0; i < geomList.size(); ++i )
  {
    geosGeometries[i] = asGeos( geomList.at( i ), mPrecision );
  }

  GEOSGeometry* geomUnion = nullptr;
  try
  {
    GEOSGeometry* geomCollection =  createGeosCollection( GEOS_GEOMETRYCOLLECTION, geosGeometries );
    geomUnion = GEOSUnaryUnion_r( geosinit.ctxt, geomCollection );
    GEOSGeom_destroy_r( geosinit.ctxt, geomCollection );
  }
  CATCH_GEOS_WITH_ERRMSG( nullptr )

  QgsAbstractGeometryV2* result = fromGeos( geomUnion );
  GEOSGeom_destroy_r( geosinit.ctxt, geomUnion );
  return result;
}

QgsAbstractGeometryV2* QgsGeos::symDifference( const QgsAbstractGeometryV2& geom, QString* errorMsg ) const
{
  return overlay( geom, SYMDIFFERENCE, errorMsg );
}

double QgsGeos::distance( const QgsAbstractGeometryV2& geom, QString* errorMsg ) const
{
  double distance = -1.0;
  if ( !mGeos )
  {
    return distance;
  }

  GEOSGeometry* otherGeosGeom = asGeos( &geom, mPrecision );
  if ( !otherGeosGeom )
  {
    return distance;
  }

  try
  {
    GEOSDistance_r( geosinit.ctxt, mGeos, otherGeosGeom, &distance );
  }
  CATCH_GEOS_WITH_ERRMSG( -1.0 )

  GEOSGeom_destroy_r( geosinit.ctxt, otherGeosGeom );

  return distance;
}

bool QgsGeos::intersects( const QgsAbstractGeometryV2& geom, QString* errorMsg ) const
{
  return relation( geom, INTERSECTS, errorMsg );
}

bool QgsGeos::touches( const QgsAbstractGeometryV2& geom, QString* errorMsg ) const
{
  return relation( geom, TOUCHES, errorMsg );
}

bool QgsGeos::crosses( const QgsAbstractGeometryV2& geom, QString* errorMsg ) const
{
  return relation( geom, CROSSES, errorMsg );
}

bool QgsGeos::within( const QgsAbstractGeometryV2& geom, QString* errorMsg ) const
{
  return relation( geom, WITHIN, errorMsg );
}

bool QgsGeos::overlaps( const QgsAbstractGeometryV2& geom, QString* errorMsg ) const
{
  return relation( geom, OVERLAPS, errorMsg );
}

bool QgsGeos::contains( const QgsAbstractGeometryV2& geom, QString* errorMsg ) const
{
  return relation( geom, CONTAINS, errorMsg );
}

bool QgsGeos::disjoint( const QgsAbstractGeometryV2& geom, QString* errorMsg ) const
{
  return relation( geom, DISJOINT, errorMsg );
}

QString QgsGeos::relate( const QgsAbstractGeometryV2& geom, QString* errorMsg ) const
{
  if ( !mGeos )
  {
    return QString();
  }

  GEOSGeomScopedPtr geosGeom( asGeos( &geom, mPrecision ) );
  if ( !geosGeom )
  {
    return QString();
  }

  QString result;
  try
  {
    char* r = GEOSRelate_r( geosinit.ctxt, mGeos, geosGeom.get() );
    if ( r )
    {
      result = QString( r );
      GEOSFree_r( geosinit.ctxt, r );
    }
  }
  catch ( GEOSException &e )
  {
    if ( errorMsg )
    {
      *errorMsg = e.what();
    }
  }

  return result;
}

bool QgsGeos::relatePattern( const QgsAbstractGeometryV2& geom, const QString& pattern, QString* errorMsg ) const
{
  if ( !mGeos )
  {
    return false;
  }

  GEOSGeomScopedPtr geosGeom( asGeos( &geom, mPrecision ) );
  if ( !geosGeom )
  {
    return false;
  }

  bool result = false;
  try
  {
    result = ( GEOSRelatePattern_r( geosinit.ctxt, mGeos, geosGeom.get(), pattern.toLocal8Bit().constData() ) == 1 );
  }
  catch ( GEOSException &e )
  {
    if ( errorMsg )
    {
      *errorMsg = e.what();
    }
  }

  return result;
}

double QgsGeos::area( QString* errorMsg ) const
{
  double area = -1.0;
  if ( !mGeos )
  {
    return area;
  }

  try
  {
    if ( GEOSArea_r( geosinit.ctxt, mGeos, &area ) != 1 )
      return -1.0;
  }
  CATCH_GEOS_WITH_ERRMSG( -1.0 );
  return area;
}

double QgsGeos::length( QString* errorMsg ) const
{
  double length = -1.0;
  if ( !mGeos )
  {
    return length;
  }
  try
  {
    if ( GEOSLength_r( geosinit.ctxt, mGeos, &length ) != 1 )
      return -1.0;
  }
  CATCH_GEOS_WITH_ERRMSG( -1.0 )
  return length;
}

int QgsGeos::splitGeometry( const QgsLineStringV2& splitLine,
                            QList<QgsAbstractGeometryV2*>&newGeometries,
                            bool topological,
                            QgsPointSequenceV2 &topologyTestPoints,
                            QString* errorMsg ) const
{

  int returnCode = 0;
  if ( !mGeometry || !mGeos )
  {
    return 1;
  }

  //return if this type is point/multipoint
  if ( mGeometry->dimension() == 0 )
  {
    return 1; //cannot split points
  }

  if ( !GEOSisValid_r( geosinit.ctxt, mGeos ) )
    return 7;

  //make sure splitLine is valid
  if (( mGeometry->dimension() == 1 && splitLine.numPoints() < 1 ) ||
      ( mGeometry->dimension() == 2 && splitLine.numPoints() < 2 ) )
    return 1;

  newGeometries.clear();
  GEOSGeometry* splitLineGeos = nullptr;

  try
  {
    if ( splitLine.numPoints() > 1 )
    {
      splitLineGeos = createGeosLinestring( &splitLine, mPrecision );
    }
    else if ( splitLine.numPoints() == 1 )
    {
      QgsPointV2  pt = splitLine.pointN( 0 );
      splitLineGeos = createGeosPoint( &pt, 2, mPrecision );
    }
    else
    {
      return 1;
    }

    if ( !GEOSisValid_r( geosinit.ctxt, splitLineGeos ) || !GEOSisSimple_r( geosinit.ctxt, splitLineGeos ) )
    {
      GEOSGeom_destroy_r( geosinit.ctxt, splitLineGeos );
      return 1;
    }

    if ( topological )
    {
      //find out candidate points for topological corrections
      if ( topologicalTestPointsSplit( splitLineGeos, topologyTestPoints ) != 0 )
        return 1;
    }

    //call split function depending on geometry type
    if ( mGeometry->dimension() == 1 )
    {
      returnCode = splitLinearGeometry( splitLineGeos, newGeometries );
      GEOSGeom_destroy_r( geosinit.ctxt, splitLineGeos );
    }
    else if ( mGeometry->dimension() == 2 )
    {
      returnCode = splitPolygonGeometry( splitLineGeos, newGeometries );
      GEOSGeom_destroy_r( geosinit.ctxt, splitLineGeos );
    }
    else
    {
      return 1;
    }
  }
  CATCH_GEOS_WITH_ERRMSG( 2 )

  return returnCode;
}



int QgsGeos::topologicalTestPointsSplit( const GEOSGeometry* splitLine, QgsPointSequenceV2 &testPoints, QString* errorMsg ) const
{
  //Find out the intersection points between splitLineGeos and this geometry.
  //These points need to be tested for topological correctness by the calling function
  //if topological editing is enabled

  if ( !mGeos )
  {
    return 1;
  }

  try
  {
    testPoints.clear();
    GEOSGeometry* intersectionGeom = GEOSIntersection_r( geosinit.ctxt, mGeos, splitLine );
    if ( !intersectionGeom )
      return 1;

    bool simple = false;
    int nIntersectGeoms = 1;
    if ( GEOSGeomTypeId_r( geosinit.ctxt, intersectionGeom ) == GEOS_LINESTRING
         || GEOSGeomTypeId_r( geosinit.ctxt, intersectionGeom ) == GEOS_POINT )
      simple = true;

    if ( !simple )
      nIntersectGeoms = GEOSGetNumGeometries_r( geosinit.ctxt, intersectionGeom );

    for ( int i = 0; i < nIntersectGeoms; ++i )
    {
      const GEOSGeometry* currentIntersectGeom;
      if ( simple )
        currentIntersectGeom = intersectionGeom;
      else
        currentIntersectGeom = GEOSGetGeometryN_r( geosinit.ctxt, intersectionGeom, i );

      const GEOSCoordSequence* lineSequence = GEOSGeom_getCoordSeq_r( geosinit.ctxt, currentIntersectGeom );
      unsigned int sequenceSize = 0;
      double x, y;
      if ( GEOSCoordSeq_getSize_r( geosinit.ctxt, lineSequence, &sequenceSize ) != 0 )
      {
        for ( unsigned int i = 0; i < sequenceSize; ++i )
        {
          if ( GEOSCoordSeq_getX_r( geosinit.ctxt, lineSequence, i, &x ) != 0 )
          {
            if ( GEOSCoordSeq_getY_r( geosinit.ctxt, lineSequence, i, &y ) != 0 )
            {
              testPoints.push_back( QgsPointV2( x, y ) );
            }
          }
        }
      }
    }
    GEOSGeom_destroy_r( geosinit.ctxt, intersectionGeom );
  }
  CATCH_GEOS_WITH_ERRMSG( 1 )

  return 0;
}

GEOSGeometry* QgsGeos::linePointDifference( GEOSGeometry* GEOSsplitPoint ) const
{
  int type = GEOSGeomTypeId_r( geosinit.ctxt, mGeos );

  QgsMultiCurveV2* multiCurve = nullptr;
  if ( type == GEOS_MULTILINESTRING )
  {
    multiCurve = dynamic_cast<QgsMultiCurveV2*>( mGeometry->clone() );
  }
  else if ( type == GEOS_LINESTRING )
  {
    multiCurve = new QgsMultiCurveV2();
    multiCurve->addGeometry( mGeometry->clone() );
  }
  else
  {
    return nullptr;
  }

  if ( !multiCurve )
  {
    return nullptr;
  }


  QgsAbstractGeometryV2* splitGeom = fromGeos( GEOSsplitPoint );
  QgsPointV2* splitPoint = dynamic_cast<QgsPointV2*>( splitGeom );
  if ( !splitPoint )
  {
    delete splitGeom;
    return nullptr;
  }

  QgsMultiCurveV2 lines;

  //For each part
  for ( int i = 0; i < multiCurve->numGeometries(); ++i )
  {
    const QgsLineStringV2* line = dynamic_cast<const QgsLineStringV2*>( multiCurve->geometryN( i ) );
    if ( line )
    {
      //For each segment
      QgsLineStringV2 newLine;
      newLine.addVertex( line->pointN( 0 ) );
      int nVertices = line->numPoints();
      for ( int j = 1; j < ( nVertices - 1 ); ++j )
      {
        QgsPointV2 currentPoint = line->pointN( j );
        newLine.addVertex( currentPoint );
        if ( currentPoint == *splitPoint )
        {
          lines.addGeometry( newLine.clone() );
          newLine = QgsLineStringV2();
          newLine.addVertex( currentPoint );
        }
      }
      newLine.addVertex( line->pointN( nVertices - 1 ) );
      lines.addGeometry( newLine.clone() );
    }
  }

  delete splitGeom;
  delete multiCurve;
  return asGeos( &lines, mPrecision );
}

int QgsGeos::splitLinearGeometry( GEOSGeometry* splitLine, QList<QgsAbstractGeometryV2*>& newGeometries ) const
{
  if ( !splitLine )
    return 2;

  if ( !mGeos )
    return 5;

  //first test if linestring intersects geometry. If not, return straight away
  if ( !GEOSIntersects_r( geosinit.ctxt, splitLine, mGeos ) )
    return 1;

  //check that split line has no linear intersection
  int linearIntersect = GEOSRelatePattern_r( geosinit.ctxt, mGeos, splitLine, "1********" );
  if ( linearIntersect > 0 )
    return 3;

  int splitGeomType = GEOSGeomTypeId_r( geosinit.ctxt, splitLine );

  GEOSGeometry* splitGeom;
  if ( splitGeomType == GEOS_POINT )
  {
    splitGeom = linePointDifference( splitLine );
  }
  else
  {
    splitGeom = GEOSDifference_r( geosinit.ctxt, mGeos, splitLine );
  }
  QVector<GEOSGeometry*> lineGeoms;

  int splitType = GEOSGeomTypeId_r( geosinit.ctxt, splitGeom );
  if ( splitType == GEOS_MULTILINESTRING )
  {
    int nGeoms = GEOSGetNumGeometries_r( geosinit.ctxt, splitGeom );
    lineGeoms.reserve( nGeoms );
    for ( int i = 0; i < nGeoms; ++i )
      lineGeoms << GEOSGeom_clone_r( geosinit.ctxt, GEOSGetGeometryN_r( geosinit.ctxt, splitGeom, i ) );

  }
  else
  {
    lineGeoms << GEOSGeom_clone_r( geosinit.ctxt, splitGeom );
  }

  mergeGeometriesMultiTypeSplit( lineGeoms );

  for ( int i = 0; i < lineGeoms.size(); ++i )
  {
    newGeometries << fromGeos( lineGeoms[i] );
    GEOSGeom_destroy_r( geosinit.ctxt, lineGeoms[i] );
  }

  GEOSGeom_destroy_r( geosinit.ctxt, splitGeom );
  return 0;
}

int QgsGeos::splitPolygonGeometry( GEOSGeometry* splitLine, QList<QgsAbstractGeometryV2*>& newGeometries ) const
{
  if ( !splitLine )
    return 2;

  if ( !mGeos )
    return 5;

  //first test if linestring intersects geometry. If not, return straight away
  if ( !GEOSIntersects_r( geosinit.ctxt, splitLine, mGeos ) )
    return 1;

  //first union all the polygon rings together (to get them noded, see JTS developer guide)
  GEOSGeometry *nodedGeometry = nodeGeometries( splitLine, mGeos );
  if ( !nodedGeometry )
    return 2; //an error occurred during noding

  GEOSGeometry *polygons = GEOSPolygonize_r( geosinit.ctxt, &nodedGeometry, 1 );
  if ( !polygons || numberOfGeometries( polygons ) == 0 )
  {
    if ( polygons )
      GEOSGeom_destroy_r( geosinit.ctxt, polygons );

    GEOSGeom_destroy_r( geosinit.ctxt, nodedGeometry );

    return 4;
  }

  GEOSGeom_destroy_r( geosinit.ctxt, nodedGeometry );

  //test every polygon if contained in original geometry
  //include in result if yes
  QVector<GEOSGeometry*> testedGeometries;
  GEOSGeometry *intersectGeometry = nullptr;

  //ratio intersect geometry / geometry. This should be close to 1
  //if the polygon belongs to the input geometry

  for ( int i = 0; i < numberOfGeometries( polygons ); i++ )
  {
    const GEOSGeometry *polygon = GEOSGetGeometryN_r( geosinit.ctxt, polygons, i );
    intersectGeometry = GEOSIntersection_r( geosinit.ctxt, mGeos, polygon );
    if ( !intersectGeometry )
    {
      QgsDebugMsg( "intersectGeometry is nullptr" );
      continue;
    }

    double intersectionArea;
    GEOSArea_r( geosinit.ctxt, intersectGeometry, &intersectionArea );

    double polygonArea;
    GEOSArea_r( geosinit.ctxt, polygon, &polygonArea );

    const double areaRatio = intersectionArea / polygonArea;
    if ( areaRatio > 0.99 && areaRatio < 1.01 )
      testedGeometries << GEOSGeom_clone_r( geosinit.ctxt, polygon );

    GEOSGeom_destroy_r( geosinit.ctxt, intersectGeometry );
  }
  GEOSGeom_destroy_r( geosinit.ctxt, polygons );

  bool splitDone = true;
  int nGeometriesThis = numberOfGeometries( mGeos ); //original number of geometries
  if ( testedGeometries.size() == nGeometriesThis )
  {
    splitDone = false;
  }

  mergeGeometriesMultiTypeSplit( testedGeometries );

  //no split done, preserve original geometry
  if ( !splitDone )
  {
    for ( int i = 0; i < testedGeometries.size(); ++i )
    {
      GEOSGeom_destroy_r( geosinit.ctxt, testedGeometries[i] );
    }
    return 1;
  }

  int i;
  for ( i = 0; i < testedGeometries.size() && GEOSisValid_r( geosinit.ctxt, testedGeometries[i] ); ++i )
    ;

  if ( i < testedGeometries.size() )
  {
    for ( i = 0; i < testedGeometries.size(); ++i )
      GEOSGeom_destroy_r( geosinit.ctxt, testedGeometries[i] );

    return 3;
  }

  for ( i = 0; i < testedGeometries.size(); ++i )
    newGeometries << fromGeos( testedGeometries[i] );

  return 0;
}

GEOSGeometry* QgsGeos::nodeGeometries( const GEOSGeometry *splitLine, const GEOSGeometry *geom )
{
  if ( !splitLine || !geom )
    return nullptr;

  GEOSGeometry *geometryBoundary = nullptr;
  if ( GEOSGeomTypeId_r( geosinit.ctxt, geom ) == GEOS_POLYGON || GEOSGeomTypeId_r( geosinit.ctxt, geom ) == GEOS_MULTIPOLYGON )
    geometryBoundary = GEOSBoundary_r( geosinit.ctxt, geom );
  else
    geometryBoundary = GEOSGeom_clone_r( geosinit.ctxt, geom );

  GEOSGeometry *splitLineClone = GEOSGeom_clone_r( geosinit.ctxt, splitLine );
  GEOSGeometry *unionGeometry = GEOSUnion_r( geosinit.ctxt, splitLineClone, geometryBoundary );
  GEOSGeom_destroy_r( geosinit.ctxt, splitLineClone );

  GEOSGeom_destroy_r( geosinit.ctxt, geometryBoundary );
  return unionGeometry;
}

int QgsGeos::mergeGeometriesMultiTypeSplit( QVector<GEOSGeometry*>& splitResult ) const
{
  if ( !mGeos )
    return 1;

  //convert mGeos to geometry collection
  int type = GEOSGeomTypeId_r( geosinit.ctxt, mGeos );
  if ( type != GEOS_GEOMETRYCOLLECTION &&
       type != GEOS_MULTILINESTRING &&
       type != GEOS_MULTIPOLYGON &&
       type != GEOS_MULTIPOINT )
    return 0;

  QVector<GEOSGeometry*> copyList = splitResult;
  splitResult.clear();

  //collect all the geometries that belong to the initial multifeature
  QVector<GEOSGeometry*> unionGeom;

  for ( int i = 0; i < copyList.size(); ++i )
  {
    //is this geometry a part of the original multitype?
    bool isPart = false;
    for ( int j = 0; j < GEOSGetNumGeometries_r( geosinit.ctxt, mGeos ); j++ )
    {
      if ( GEOSEquals_r( geosinit.ctxt, copyList[i], GEOSGetGeometryN_r( geosinit.ctxt, mGeos, j ) ) )
      {
        isPart = true;
        break;
      }
    }

    if ( isPart )
    {
      unionGeom << copyList[i];
    }
    else
    {
      QVector<GEOSGeometry*> geomVector;
      geomVector << copyList[i];

      if ( type == GEOS_MULTILINESTRING )
        splitResult << createGeosCollection( GEOS_MULTILINESTRING, geomVector );
      else if ( type == GEOS_MULTIPOLYGON )
        splitResult << createGeosCollection( GEOS_MULTIPOLYGON, geomVector );
      else
        GEOSGeom_destroy_r( geosinit.ctxt, copyList[i] );
    }
  }

  //make multifeature out of unionGeom
  if ( !unionGeom.isEmpty() )
  {
    if ( type == GEOS_MULTILINESTRING )
      splitResult << createGeosCollection( GEOS_MULTILINESTRING, unionGeom );
    else if ( type == GEOS_MULTIPOLYGON )
      splitResult << createGeosCollection( GEOS_MULTIPOLYGON, unionGeom );
  }
  else
  {
    unionGeom.clear();
  }

  return 0;
}

GEOSGeometry* QgsGeos::createGeosCollection( int typeId, const QVector<GEOSGeometry*>& geoms )
{
  int nNullGeoms = geoms.count( nullptr );
  int nNotNullGeoms = geoms.size() - nNullGeoms;

  GEOSGeometry **geomarr = new GEOSGeometry*[ nNotNullGeoms ];
  if ( !geomarr )
  {
    return nullptr;
  }

  int i = 0;
  QVector<GEOSGeometry*>::const_iterator geomIt = geoms.constBegin();
  for ( ; geomIt != geoms.constEnd(); ++geomIt )
  {
    if ( *geomIt )
    {
      geomarr[i] = *geomIt;
      ++i;
    }
  }
  GEOSGeometry *geom = nullptr;

  try
  {
    geom = GEOSGeom_createCollection_r( geosinit.ctxt, typeId, geomarr, nNotNullGeoms );
  }
  catch ( GEOSException &e )
  {
    QgsMessageLog::logMessage( QObject::tr( "Exception: %1" ).arg( e.what() ), QObject::tr( "GEOS" ) );
  }

  delete [] geomarr;

  return geom;
}

QgsAbstractGeometryV2* QgsGeos::fromGeos( const GEOSGeometry* geos )
{
  if ( !geos )
  {
    return nullptr;
  }

  int nCoordDims = GEOSGeom_getCoordinateDimension_r( geosinit.ctxt, geos );
  int nDims = GEOSGeom_getDimensions_r( geosinit.ctxt, geos );
  bool hasZ = ( nCoordDims == 3 );
  bool hasM = (( nDims - nCoordDims ) == 1 );

  switch ( GEOSGeomTypeId_r( geosinit.ctxt, geos ) )
  {
    case GEOS_POINT:                 // a point
    {
      const GEOSCoordSequence* cs = GEOSGeom_getCoordSeq_r( geosinit.ctxt, geos );
      return ( coordSeqPoint( cs, 0, hasZ, hasM ).clone() );
    }
    case GEOS_LINESTRING:
    {
      return sequenceToLinestring( geos, hasZ, hasM );
    }
    case GEOS_POLYGON:
    {
      return fromGeosPolygon( geos );
    }
    case GEOS_MULTIPOINT:
    {
      QgsMultiPointV2* multiPoint = new QgsMultiPointV2();
      int nParts = GEOSGetNumGeometries_r( geosinit.ctxt, geos );
      for ( int i = 0; i < nParts; ++i )
      {
        const GEOSCoordSequence* cs = GEOSGeom_getCoordSeq_r( geosinit.ctxt, GEOSGetGeometryN_r( geosinit.ctxt, geos, i ) );
        if ( cs )
        {
          multiPoint->addGeometry( coordSeqPoint( cs, 0, hasZ, hasM ).clone() );
        }
      }
      return multiPoint;
    }
    case GEOS_MULTILINESTRING:
    {
      QgsMultiLineStringV2* multiLineString = new QgsMultiLineStringV2();
      int nParts = GEOSGetNumGeometries_r( geosinit.ctxt, geos );
      for ( int i = 0; i < nParts; ++i )
      {
        QgsLineStringV2* line = sequenceToLinestring( GEOSGetGeometryN_r( geosinit.ctxt, geos, i ), hasZ, hasM );
        if ( line )
        {
          multiLineString->addGeometry( line );
        }
      }
      return multiLineString;
    }
    case GEOS_MULTIPOLYGON:
    {
      QgsMultiPolygonV2* multiPolygon = new QgsMultiPolygonV2();

      int nParts = GEOSGetNumGeometries_r( geosinit.ctxt, geos );
      for ( int i = 0; i < nParts; ++i )
      {
        QgsPolygonV2* poly = fromGeosPolygon( GEOSGetGeometryN_r( geosinit.ctxt, geos, i ) );
        if ( poly )
        {
          multiPolygon->addGeometry( poly );
        }
      }
      return multiPolygon;
    }
    case GEOS_GEOMETRYCOLLECTION:
    {
      QgsGeometryCollectionV2* geomCollection = new QgsGeometryCollectionV2();
      int nParts = GEOSGetNumGeometries_r( geosinit.ctxt, geos );
      for ( int i = 0; i < nParts; ++i )
      {
        QgsAbstractGeometryV2* geom = fromGeos( GEOSGetGeometryN_r( geosinit.ctxt, geos, i ) );
        if ( geom )
        {
          geomCollection->addGeometry( geom );
        }
      }
      return geomCollection;
    }
  }
  return nullptr;
}

QgsPolygonV2* QgsGeos::fromGeosPolygon( const GEOSGeometry* geos )
{
  if ( GEOSGeomTypeId_r( geosinit.ctxt, geos ) != GEOS_POLYGON )
  {
    return nullptr;
  }

  int nCoordDims = GEOSGeom_getCoordinateDimension_r( geosinit.ctxt, geos );
  int nDims = GEOSGeom_getDimensions_r( geosinit.ctxt, geos );
  bool hasZ = ( nCoordDims == 3 );
  bool hasM = (( nDims - nCoordDims ) == 1 );

  QgsPolygonV2* polygon = new QgsPolygonV2();

  const GEOSGeometry* ring = GEOSGetExteriorRing_r( geosinit.ctxt, geos );
  if ( ring )
  {
    polygon->setExteriorRing( sequenceToLinestring( ring, hasZ, hasM ) );
  }

  QList<QgsCurveV2*> interiorRings;
  for ( int i = 0; i < GEOSGetNumInteriorRings_r( geosinit.ctxt, geos ); ++i )
  {
    ring = GEOSGetInteriorRingN_r( geosinit.ctxt, geos, i );
    if ( ring )
    {
      interiorRings.push_back( sequenceToLinestring( ring, hasZ, hasM ) );
    }
  }
  polygon->setInteriorRings( interiorRings );

  return polygon;
}

QgsLineStringV2* QgsGeos::sequenceToLinestring( const GEOSGeometry* geos, bool hasZ, bool hasM )
{
  QgsPointSequenceV2 pts;
  const GEOSCoordSequence* cs = GEOSGeom_getCoordSeq_r( geosinit.ctxt, geos );
  unsigned int nPoints;
  GEOSCoordSeq_getSize_r( geosinit.ctxt, cs, &nPoints );
  pts.reserve( nPoints );
  for ( unsigned int i = 0; i < nPoints; ++i )
  {
    pts.push_back( coordSeqPoint( cs, i, hasZ, hasM ) );
  }
  QgsLineStringV2* line = new QgsLineStringV2();
  line->setPoints( pts );
  return line;
}

int QgsGeos::numberOfGeometries( GEOSGeometry* g )
{
  if ( !g )
    return 0;

  int geometryType = GEOSGeomTypeId_r( geosinit.ctxt, g );
  if ( geometryType == GEOS_POINT || geometryType == GEOS_LINESTRING || geometryType == GEOS_LINEARRING
       || geometryType == GEOS_POLYGON )
    return 1;

  //calling GEOSGetNumGeometries is save for multi types and collections also in geos2
  return GEOSGetNumGeometries_r( geosinit.ctxt, g );
}

QgsPointV2 QgsGeos::coordSeqPoint( const GEOSCoordSequence* cs, int i, bool hasZ, bool hasM )
{
  if ( !cs )
  {
    return QgsPointV2();
  }

  double x, y;
  double z = 0;
  double m = 0;
  GEOSCoordSeq_getX_r( geosinit.ctxt, cs, i, &x );
  GEOSCoordSeq_getY_r( geosinit.ctxt, cs, i, &y );
  if ( hasZ )
  {
    GEOSCoordSeq_getZ_r( geosinit.ctxt, cs, i, &z );
  }
  if ( hasM )
  {
    GEOSCoordSeq_getOrdinate_r( geosinit.ctxt, cs, i, 3, &m );
  }

  QgsWKBTypes::Type t = QgsWKBTypes::Point;
  if ( hasZ && hasM )
  {
    t = QgsWKBTypes::PointZM;
  }
  else if ( hasZ )
  {
    t = QgsWKBTypes::PointZ;
  }
  else if ( hasM )
  {
    t = QgsWKBTypes::PointM;
  }
  return QgsPointV2( t, x, y, z, m );
}

GEOSGeometry* QgsGeos::asGeos( const QgsAbstractGeometryV2* geom, double precision )
{
  int coordDims = 2;
  if ( geom->is3D() )
  {
    ++coordDims;
  }
  if ( geom->isMeasure() )
  {
    ++coordDims;
  }

  if ( QgsWKBTypes::isMultiType( geom->wkbType() )  || QgsWKBTypes::flatType( geom->wkbType() ) == QgsWKBTypes::GeometryCollection )
  {
    int geosType;

    if ( QgsWKBTypes::flatType( geom->wkbType() ) == QgsWKBTypes::GeometryCollection )
      geosType = GEOS_GEOMETRYCOLLECTION;
    else
    {
      switch ( QgsWKBTypes::geometryType( geom->wkbType() ) )
      {
        case QgsWKBTypes::PointGeometry:
          geosType = GEOS_MULTIPOINT;
          break;

        case QgsWKBTypes::LineGeometry:
          geosType = GEOS_MULTILINESTRING;
          break;

        case QgsWKBTypes::PolygonGeometry:
          geosType = GEOS_MULTIPOLYGON;
          break;

        case QgsWKBTypes::UnknownGeometry:
        case QgsWKBTypes::NullGeometry:
        default:
          return nullptr;
          break;
      }
    }

    const QgsGeometryCollectionV2* c = dynamic_cast<const QgsGeometryCollectionV2*>( geom );
    if ( !c )
      return nullptr;

    QVector< GEOSGeometry* > geomVector( c->numGeometries() );
    for ( int i = 0; i < c->numGeometries(); ++i )
    {
      geomVector[i] = asGeos( c->geometryN( i ), precision );
    }
    return createGeosCollection( geosType, geomVector );
  }
  else
  {
    switch ( QgsWKBTypes::geometryType( geom->wkbType() ) )
    {
      case QgsWKBTypes::PointGeometry:
        return createGeosPoint( static_cast<const QgsPointV2*>( geom ), coordDims, precision );
        break;

      case QgsWKBTypes::LineGeometry:
        return createGeosLinestring( static_cast<const QgsLineStringV2*>( geom ), precision );
        break;

      case QgsWKBTypes::PolygonGeometry:
        return createGeosPolygon( static_cast<const QgsPolygonV2*>( geom ), precision );
        break;

      case QgsWKBTypes::UnknownGeometry:
      case QgsWKBTypes::NullGeometry:
        return nullptr;
        break;
    }
  }
  return nullptr;
}

QgsAbstractGeometryV2* QgsGeos::overlay( const QgsAbstractGeometryV2& geom, Overlay op, QString* errorMsg ) const
{
  if ( !mGeos )
  {
    return nullptr;
  }

  GEOSGeomScopedPtr geosGeom( asGeos( &geom, mPrecision ) );
  if ( !geosGeom )
  {
    return nullptr;
  }

  try
  {
    GEOSGeomScopedPtr opGeom;
    switch ( op )
    {
      case INTERSECTION:
        opGeom.reset( GEOSIntersection_r( geosinit.ctxt, mGeos, geosGeom.get() ) );
        break;
      case DIFFERENCE:
        opGeom.reset( GEOSDifference_r( geosinit.ctxt, mGeos, geosGeom.get() ) );
        break;
      case UNION:
      {
        GEOSGeometry *unionGeometry = GEOSUnion_r( geosinit.ctxt, mGeos, geosGeom.get() );

        if ( unionGeometry && GEOSGeomTypeId_r( geosinit.ctxt, unionGeometry ) == GEOS_MULTILINESTRING )
        {
          GEOSGeometry *mergedLines = GEOSLineMerge_r( geosinit.ctxt, unionGeometry );
          if ( mergedLines )
          {
            GEOSGeom_destroy_r( geosinit.ctxt, unionGeometry );
            unionGeometry = mergedLines;
          }
        }

        opGeom.reset( unionGeometry );
      }
      break;
      case SYMDIFFERENCE:
        opGeom.reset( GEOSSymDifference_r( geosinit.ctxt, mGeos, geosGeom.get() ) );
        break;
      default:    //unknown op
        return nullptr;
    }
    QgsAbstractGeometryV2* opResult = fromGeos( opGeom.get() );
    return opResult;
  }
  catch ( GEOSException &e )
  {
    if ( errorMsg )
    {
      *errorMsg = e.what();
    }
    return nullptr;
  }
}

bool QgsGeos::relation( const QgsAbstractGeometryV2& geom, Relation r, QString* errorMsg ) const
{
  if ( !mGeos )
  {
    return false;
  }

  GEOSGeomScopedPtr geosGeom( asGeos( &geom, mPrecision ) );
  if ( !geosGeom )
  {
    return false;
  }

  bool result = false;
  try
  {
    if ( mGeosPrepared ) //use faster version with prepared geometry
    {
      switch ( r )
      {
        case INTERSECTS:
          result = ( GEOSPreparedIntersects_r( geosinit.ctxt, mGeosPrepared, geosGeom.get() ) == 1 );
          break;
        case TOUCHES:
          result = ( GEOSPreparedTouches_r( geosinit.ctxt, mGeosPrepared, geosGeom.get() ) == 1 );
          break;
        case CROSSES:
          result = ( GEOSPreparedCrosses_r( geosinit.ctxt, mGeosPrepared, geosGeom.get() ) == 1 );
          break;
        case WITHIN:
          result = ( GEOSPreparedWithin_r( geosinit.ctxt, mGeosPrepared, geosGeom.get() ) == 1 );
          break;
        case CONTAINS:
          result = ( GEOSPreparedContains_r( geosinit.ctxt, mGeosPrepared, geosGeom.get() ) == 1 );
          break;
        case DISJOINT:
          result = ( GEOSPreparedDisjoint_r( geosinit.ctxt, mGeosPrepared, geosGeom.get() ) == 1 );
          break;
        case OVERLAPS:
          result = ( GEOSPreparedOverlaps_r( geosinit.ctxt, mGeosPrepared, geosGeom.get() ) == 1 );
          break;
        default:
          return false;
      }
      return result;
    }

    switch ( r )
    {
      case INTERSECTS:
        result = ( GEOSIntersects_r( geosinit.ctxt, mGeos, geosGeom.get() ) == 1 );
        break;
      case TOUCHES:
        result = ( GEOSTouches_r( geosinit.ctxt, mGeos, geosGeom.get() ) == 1 );
        break;
      case CROSSES:
        result = ( GEOSCrosses_r( geosinit.ctxt, mGeos, geosGeom.get() ) == 1 );
        break;
      case WITHIN:
        result = ( GEOSWithin_r( geosinit.ctxt, mGeos, geosGeom.get() ) == 1 );
        break;
      case CONTAINS:
        result = ( GEOSContains_r( geosinit.ctxt, mGeos, geosGeom.get() ) == 1 );
        break;
      case DISJOINT:
        result = ( GEOSDisjoint_r( geosinit.ctxt, mGeos, geosGeom.get() ) == 1 );
        break;
      case OVERLAPS:
        result = ( GEOSOverlaps_r( geosinit.ctxt, mGeos, geosGeom.get() ) == 1 );
        break;
      default:
        return false;
    }
  }
  catch ( GEOSException &e )
  {
    if ( errorMsg )
    {
      *errorMsg = e.what();
    }
    return 0;
  }

  return result;
}

QgsAbstractGeometryV2* QgsGeos::buffer( double distance, int segments, QString* errorMsg ) const
{
  if ( !mGeos )
  {
    return nullptr;
  }

  GEOSGeomScopedPtr geos;
  try
  {
    geos.reset( GEOSBuffer_r( geosinit.ctxt, mGeos, distance, segments ) );
  }
  CATCH_GEOS_WITH_ERRMSG( nullptr );
  return fromGeos( geos.get() );
}

QgsAbstractGeometryV2 *QgsGeos::buffer( double distance, int segments, int endCapStyle, int joinStyle, double mitreLimit, QString* errorMsg ) const
{
  if ( !mGeos )
  {
    return nullptr;
  }

#if defined(GEOS_VERSION_MAJOR) && defined(GEOS_VERSION_MINOR) && \
 ((GEOS_VERSION_MAJOR>3) || ((GEOS_VERSION_MAJOR==3) && (GEOS_VERSION_MINOR>=3)))

  GEOSGeomScopedPtr geos;
  try
  {
    geos.reset( GEOSBufferWithStyle_r( geosinit.ctxt, mGeos, distance, segments, endCapStyle, joinStyle, mitreLimit ) );
  }
  CATCH_GEOS_WITH_ERRMSG( nullptr );
  return fromGeos( geos.get() );
#else
  return 0;
#endif //0
}

QgsAbstractGeometryV2* QgsGeos::simplify( double tolerance, QString* errorMsg ) const
{
  if ( !mGeos )
  {
    return nullptr;
  }
  GEOSGeomScopedPtr geos;
  try
  {
    geos.reset( GEOSTopologyPreserveSimplify_r( geosinit.ctxt, mGeos, tolerance ) );
  }
  CATCH_GEOS_WITH_ERRMSG( nullptr );
  return fromGeos( geos.get() );
}

QgsAbstractGeometryV2* QgsGeos::interpolate( double distance, QString* errorMsg ) const
{
  if ( !mGeos )
  {
    return nullptr;
  }
  GEOSGeomScopedPtr geos;
  try
  {
    geos.reset( GEOSInterpolate_r( geosinit.ctxt, mGeos, distance ) );
  }
  CATCH_GEOS_WITH_ERRMSG( nullptr );
  return fromGeos( geos.get() );
}

bool QgsGeos::centroid( QgsPointV2& pt, QString* errorMsg ) const
{
  if ( !mGeos )
  {
    return false;
  }

  GEOSGeomScopedPtr geos;
  try
  {
    geos.reset( GEOSGetCentroid_r( geosinit.ctxt,  mGeos ) );
  }
  CATCH_GEOS_WITH_ERRMSG( false );

  if ( !geos )
  {
    return false;
  }

  double x, y;
  GEOSGeomGetX_r( geosinit.ctxt, geos.get(), &x );
  GEOSGeomGetY_r( geosinit.ctxt, geos.get(), &y );
  pt.setX( x );
  pt.setY( y );
  return true;
}

QgsAbstractGeometryV2* QgsGeos::envelope( QString* errorMsg ) const
{
  if ( !mGeos )
  {
    return nullptr;
  }
  GEOSGeomScopedPtr geos;
  try
  {
    geos.reset( GEOSEnvelope_r( geosinit.ctxt, mGeos ) );
  }
  CATCH_GEOS_WITH_ERRMSG( nullptr );
  return fromGeos( geos.get() );
}

bool QgsGeos::pointOnSurface( QgsPointV2& pt, QString* errorMsg ) const
{
  if ( !mGeos )
  {
    return false;
  }

  GEOSGeomScopedPtr geos;
  try
  {
    geos.reset( GEOSPointOnSurface_r( geosinit.ctxt, mGeos ) );

    if ( !geos || GEOSisEmpty_r( geosinit.ctxt, geos.get() ) != 0 )
    {
      return false;
    }

    double x, y;
    GEOSGeomGetX_r( geosinit.ctxt, geos.get(), &x );
    GEOSGeomGetY_r( geosinit.ctxt, geos.get(), &y );

    pt.setX( x );
    pt.setY( y );
  }
  CATCH_GEOS_WITH_ERRMSG( false );

  return true;
}

QgsAbstractGeometryV2* QgsGeos::convexHull( QString* errorMsg ) const
{
  if ( !mGeos )
  {
    return nullptr;
  }

  try
  {
    GEOSGeometry* cHull = GEOSConvexHull_r( geosinit.ctxt, mGeos );
    QgsAbstractGeometryV2* cHullGeom = fromGeos( cHull );
    GEOSGeom_destroy_r( geosinit.ctxt, cHull );
    return cHullGeom;
  }
  CATCH_GEOS_WITH_ERRMSG( nullptr );
}

bool QgsGeos::isValid( QString* errorMsg ) const
{
  if ( !mGeos )
  {
    return false;
  }

  try
  {
    return GEOSisValid_r( geosinit.ctxt, mGeos );
  }
  CATCH_GEOS_WITH_ERRMSG( false );
}

bool QgsGeos::isEqual( const QgsAbstractGeometryV2& geom, QString* errorMsg ) const
{
  if ( !mGeos )
  {
    return false;
  }

  try
  {
    GEOSGeomScopedPtr geosGeom( asGeos( &geom, mPrecision ) );
    if ( !geosGeom )
    {
      return false;
    }
    bool equal = GEOSEquals_r( geosinit.ctxt, mGeos, geosGeom.get() );
    return equal;
  }
  CATCH_GEOS_WITH_ERRMSG( false );
}

bool QgsGeos::isEmpty( QString* errorMsg ) const
{
  if ( !mGeos )
  {
    return false;
  }

  try
  {
    return GEOSisEmpty_r( geosinit.ctxt, mGeos );
  }
  CATCH_GEOS_WITH_ERRMSG( false );
}

GEOSCoordSequence* QgsGeos::createCoordinateSequence( const QgsCurveV2* curve, double precision, bool forceClose )
{
  bool segmentize = false;
  const QgsLineStringV2* line = dynamic_cast<const QgsLineStringV2*>( curve );

  if ( !line )
  {
    line = curve->curveToLine();
    segmentize = true;
  }

  if ( !line )
  {
    return nullptr;
  }

  bool hasZ = line->is3D();
  bool hasM = false; //line->isMeasure(); //disabled until geos supports m-coordinates
  int coordDims = 2;
  if ( hasZ )
  {
    ++coordDims;
  }
  if ( hasM )
  {
    ++coordDims;
  }

  int numPoints = line->numPoints();

  int numOutPoints = numPoints;
  if ( forceClose && ( line->pointN( 0 ) != line->pointN( numPoints - 1 ) ) )
  {
    ++numOutPoints;
  }

  GEOSCoordSequence* coordSeq = nullptr;
  try
  {
    coordSeq = GEOSCoordSeq_create_r( geosinit.ctxt, numOutPoints, coordDims );
    if ( !coordSeq )
    {
      QgsMessageLog::logMessage( QObject::tr( "Could not create coordinate sequence for %1 points in %2 dimensions" ).arg( numPoints ).arg( coordDims ), QObject::tr( "GEOS" ) );
      return nullptr;
    }
    if ( precision > 0. )
    {
      for ( int i = 0; i < numOutPoints; ++i )
      {
        const QgsPointV2 &pt = line->pointN( i % numPoints ); //todo: create method to get const point reference
        GEOSCoordSeq_setX_r( geosinit.ctxt, coordSeq, i, qgsRound( pt.x() / precision ) * precision );
        GEOSCoordSeq_setY_r( geosinit.ctxt, coordSeq, i, qgsRound( pt.y() / precision ) * precision );
        if ( hasZ )
        {
          GEOSCoordSeq_setOrdinate_r( geosinit.ctxt, coordSeq, i, 2, qgsRound( pt.z() / precision ) * precision );
        }
        if ( hasM )
        {
          GEOSCoordSeq_setOrdinate_r( geosinit.ctxt, coordSeq, i, 3, pt.m() );
        }
      }
    }
    else
    {
      for ( int i = 0; i < numOutPoints; ++i )
      {
        const QgsPointV2 &pt = line->pointN( i % numPoints ); //todo: create method to get const point reference
        GEOSCoordSeq_setX_r( geosinit.ctxt, coordSeq, i, pt.x() );
        GEOSCoordSeq_setY_r( geosinit.ctxt, coordSeq, i, pt.y() );
        if ( hasZ )
        {
          GEOSCoordSeq_setOrdinate_r( geosinit.ctxt, coordSeq, i, 2, pt.z() );
        }
        if ( hasM )
        {
          GEOSCoordSeq_setOrdinate_r( geosinit.ctxt, coordSeq, i, 3, pt.m() );
        }
      }
    }
  }
  CATCH_GEOS( nullptr )

  if ( segmentize )
  {
    delete line;
  }
  return coordSeq;
}

GEOSGeometry* QgsGeos::createGeosPoint( const QgsAbstractGeometryV2* point, int coordDims, double precision )
{
  const QgsPointV2* pt = dynamic_cast<const QgsPointV2*>( point );
  if ( !pt )
    return nullptr;

  GEOSGeometry* geosPoint = nullptr;

  try
  {
    GEOSCoordSequence* coordSeq = GEOSCoordSeq_create_r( geosinit.ctxt, 1, coordDims );
    if ( !coordSeq )
    {
      QgsMessageLog::logMessage( QObject::tr( "Could not create coordinate sequence for point with %1 dimensions" ).arg( coordDims ), QObject::tr( "GEOS" ) );
      return nullptr;
    }
    if ( precision > 0. )
    {
      GEOSCoordSeq_setX_r( geosinit.ctxt, coordSeq, 0, qgsRound( pt->x() / precision ) * precision );
      GEOSCoordSeq_setY_r( geosinit.ctxt, coordSeq, 0, qgsRound( pt->y() / precision ) * precision );
      if ( pt->is3D() )
      {
        GEOSCoordSeq_setOrdinate_r( geosinit.ctxt, coordSeq, 0, 2, qgsRound( pt->z() / precision ) * precision );
      }
    }
    else
    {
      GEOSCoordSeq_setX_r( geosinit.ctxt, coordSeq, 0, pt->x() );
      GEOSCoordSeq_setY_r( geosinit.ctxt, coordSeq, 0, pt->y() );
      if ( pt->is3D() )
      {
        GEOSCoordSeq_setOrdinate_r( geosinit.ctxt, coordSeq, 0, 2, pt->z() );
      }
    }
#if 0 //disabled until geos supports m-coordinates
    if ( pt->isMeasure() )
    {
      GEOSCoordSeq_setOrdinate_r( geosinit.ctxt, coordSeq, 0, 3, pt->m() );
    }
#endif
    geosPoint = GEOSGeom_createPoint_r( geosinit.ctxt, coordSeq );
  }
  CATCH_GEOS( nullptr )
  return geosPoint;
}

GEOSGeometry* QgsGeos::createGeosLinestring( const QgsAbstractGeometryV2* curve , double precision )
{
  const QgsCurveV2* c = dynamic_cast<const QgsCurveV2*>( curve );
  if ( !c )
    return nullptr;

  GEOSCoordSequence* coordSeq = createCoordinateSequence( c, precision );
  if ( !coordSeq )
    return nullptr;

  GEOSGeometry* geosGeom = nullptr;
  try
  {
    geosGeom = GEOSGeom_createLineString_r( geosinit.ctxt, coordSeq );
  }
  CATCH_GEOS( nullptr )
  return geosGeom;
}

GEOSGeometry* QgsGeos::createGeosPolygon( const QgsAbstractGeometryV2* poly , double precision )
{
  const QgsCurvePolygonV2* polygon = dynamic_cast<const QgsCurvePolygonV2*>( poly );
  if ( !polygon )
    return nullptr;

  const QgsCurveV2* exteriorRing = polygon->exteriorRing();
  if ( !exteriorRing )
  {
    return nullptr;
  }

  GEOSGeometry* geosPolygon = nullptr;
  try
  {
    GEOSGeometry* exteriorRingGeos = GEOSGeom_createLinearRing_r( geosinit.ctxt, createCoordinateSequence( exteriorRing, precision, true ) );


    int nHoles = polygon->numInteriorRings();
    GEOSGeometry** holes = nullptr;
    if ( nHoles > 0 )
    {
      holes = new GEOSGeometry*[ nHoles ];
    }

    for ( int i = 0; i < nHoles; ++i )
    {
      const QgsCurveV2* interiorRing = polygon->interiorRing( i );
      holes[i] = GEOSGeom_createLinearRing_r( geosinit.ctxt, createCoordinateSequence( interiorRing, precision, true ) );
    }
    geosPolygon = GEOSGeom_createPolygon_r( geosinit.ctxt, exteriorRingGeos, holes, nHoles );
    delete[] holes;
  }
  CATCH_GEOS( nullptr )

  return geosPolygon;
}

QgsAbstractGeometryV2* QgsGeos::offsetCurve( double distance, int segments, int joinStyle, double mitreLimit, QString* errorMsg ) const
{
  if ( !mGeos )
    return nullptr;

  GEOSGeometry* offset = nullptr;
  try
  {
    offset = GEOSOffsetCurve_r( geosinit.ctxt, mGeos, distance, segments, joinStyle, mitreLimit );
  }
  CATCH_GEOS_WITH_ERRMSG( nullptr )
  QgsAbstractGeometryV2* offsetGeom = fromGeos( offset );
  GEOSGeom_destroy_r( geosinit.ctxt, offset );
  return offsetGeom;
}

QgsAbstractGeometryV2* QgsGeos::reshapeGeometry( const QgsLineStringV2& reshapeWithLine, int* errorCode, QString* errorMsg ) const
{
  if ( !mGeos || reshapeWithLine.numPoints() < 2 || mGeometry->dimension() == 0 )
  {
    if ( errorCode ) { *errorCode = 1; }
    return nullptr;
  }

  GEOSGeometry* reshapeLineGeos = createGeosLinestring( &reshapeWithLine, mPrecision );

  //single or multi?
  int numGeoms = GEOSGetNumGeometries_r( geosinit.ctxt, mGeos );
  if ( numGeoms == -1 )
  {
    if ( errorCode ) { *errorCode = 1; }
    GEOSGeom_destroy_r( geosinit.ctxt, reshapeLineGeos );
    return nullptr;
  }

  bool isMultiGeom = false;
  int geosTypeId = GEOSGeomTypeId_r( geosinit.ctxt, mGeos );
  if ( geosTypeId == GEOS_MULTILINESTRING || geosTypeId == GEOS_MULTIPOLYGON )
    isMultiGeom = true;

  bool isLine = ( mGeometry->dimension() == 1 );

  if ( !isMultiGeom )
  {
    GEOSGeometry* reshapedGeometry;
    if ( isLine )
    {
      reshapedGeometry = reshapeLine( mGeos, reshapeLineGeos, mPrecision );
    }
    else
    {
      reshapedGeometry = reshapePolygon( mGeos, reshapeLineGeos, mPrecision );
    }

    if ( errorCode ) { *errorCode = 0; }
    QgsAbstractGeometryV2* reshapeResult = fromGeos( reshapedGeometry );
    GEOSGeom_destroy_r( geosinit.ctxt, reshapedGeometry );
    GEOSGeom_destroy_r( geosinit.ctxt, reshapeLineGeos );
    return reshapeResult;
  }
  else
  {
    try
    {
      //call reshape for each geometry part and replace mGeos with new geometry if reshape took place
      bool reshapeTookPlace = false;

      GEOSGeometry* currentReshapeGeometry = nullptr;
      GEOSGeometry** newGeoms = new GEOSGeometry*[numGeoms];

      for ( int i = 0; i < numGeoms; ++i )
      {
        if ( isLine )
          currentReshapeGeometry = reshapeLine( GEOSGetGeometryN_r( geosinit.ctxt, mGeos, i ), reshapeLineGeos, mPrecision );
        else
          currentReshapeGeometry = reshapePolygon( GEOSGetGeometryN_r( geosinit.ctxt, mGeos, i ), reshapeLineGeos, mPrecision );

        if ( currentReshapeGeometry )
        {
          newGeoms[i] = currentReshapeGeometry;
          reshapeTookPlace = true;
        }
        else
        {
          newGeoms[i] = GEOSGeom_clone_r( geosinit.ctxt, GEOSGetGeometryN_r( geosinit.ctxt, mGeos, i ) );
        }
      }
      GEOSGeom_destroy_r( geosinit.ctxt, reshapeLineGeos );

      GEOSGeometry* newMultiGeom = nullptr;
      if ( isLine )
      {
        newMultiGeom = GEOSGeom_createCollection_r( geosinit.ctxt, GEOS_MULTILINESTRING, newGeoms, numGeoms );
      }
      else //multipolygon
      {
        newMultiGeom = GEOSGeom_createCollection_r( geosinit.ctxt, GEOS_MULTIPOLYGON, newGeoms, numGeoms );
      }

      delete[] newGeoms;
      if ( !newMultiGeom )
      {
        if ( errorCode ) { *errorCode = 3; }
        return nullptr;
      }

      if ( reshapeTookPlace )
      {
        if ( errorCode ) { *errorCode = 0; }
        QgsAbstractGeometryV2* reshapedMultiGeom = fromGeos( newMultiGeom );
        GEOSGeom_destroy_r( geosinit.ctxt, newMultiGeom );
        return reshapedMultiGeom;
      }
      else
      {
        GEOSGeom_destroy_r( geosinit.ctxt, newMultiGeom );
        if ( errorCode ) { *errorCode = 1; }
        return nullptr;
      }
    }
    CATCH_GEOS_WITH_ERRMSG( nullptr )
  }
}

QgsGeometry QgsGeos::mergeLines( QString* errorMsg ) const
{
  if ( !mGeos )
  {
    return QgsGeometry();
  }

  if ( GEOSGeomTypeId_r( geosinit.ctxt, mGeos ) != GEOS_MULTILINESTRING )
    return QgsGeometry();

  GEOSGeomScopedPtr geos;
  try
  {
    geos.reset( GEOSLineMerge_r( geosinit.ctxt, mGeos ) );
  }
  CATCH_GEOS_WITH_ERRMSG( QgsGeometry() );
  return QgsGeometry( fromGeos( geos.get() ) );
}

QgsGeometry QgsGeos::closestPoint( const QgsGeometry& other, QString* errorMsg ) const
{
  if ( !mGeos || other.isEmpty() )
  {
    return QgsGeometry();
  }

  GEOSGeomScopedPtr otherGeom( asGeos( other.geometry(), mPrecision ) );
  if ( !otherGeom )
  {
    return QgsGeometry();
  }

  double nx = 0.0;
  double ny = 0.0;
  try
  {
    GEOSCoordSequence* nearestCoord = GEOSNearestPoints_r( geosinit.ctxt, mGeos, otherGeom.get() );

    ( void )GEOSCoordSeq_getX_r( geosinit.ctxt, nearestCoord, 0, &nx );
    ( void )GEOSCoordSeq_getY_r( geosinit.ctxt, nearestCoord, 0, &ny );
    GEOSCoordSeq_destroy_r( geosinit.ctxt, nearestCoord );
  }
  catch ( GEOSException &e )
  {
    if ( errorMsg )
    {
      *errorMsg = e.what();
    }
    return QgsGeometry();
  }

  return QgsGeometry( new QgsPointV2( nx, ny ) );
}

QgsGeometry QgsGeos::shortestLine( const QgsGeometry& other, QString* errorMsg ) const
{
  if ( !mGeos || other.isEmpty() )
  {
    return QgsGeometry();
  }

  GEOSGeomScopedPtr otherGeom( asGeos( other.geometry(), mPrecision ) );
  if ( !otherGeom )
  {
    return QgsGeometry();
  }

  double nx1 = 0.0;
  double ny1 = 0.0;
  double nx2 = 0.0;
  double ny2 = 0.0;
  try
  {
    GEOSCoordSequence* nearestCoord = GEOSNearestPoints_r( geosinit.ctxt, mGeos, otherGeom.get() );

    ( void )GEOSCoordSeq_getX_r( geosinit.ctxt, nearestCoord, 0, &nx1 );
    ( void )GEOSCoordSeq_getY_r( geosinit.ctxt, nearestCoord, 0, &ny1 );
    ( void )GEOSCoordSeq_getX_r( geosinit.ctxt, nearestCoord, 1, &nx2 );
    ( void )GEOSCoordSeq_getY_r( geosinit.ctxt, nearestCoord, 1, &ny2 );

    GEOSCoordSeq_destroy_r( geosinit.ctxt, nearestCoord );
  }
  catch ( GEOSException &e )
  {
    if ( errorMsg )
    {
      *errorMsg = e.what();
    }
    return QgsGeometry();
  }

  QgsLineStringV2* line = new QgsLineStringV2();
  line->addVertex( QgsPointV2( nx1, ny1 ) );
  line->addVertex( QgsPointV2( nx2, ny2 ) );
  return QgsGeometry( line );
}

double QgsGeos::lineLocatePoint( const QgsPointV2& point, QString* errorMsg ) const
{
  if ( !mGeos )
  {
    return -1;
  }

  GEOSGeomScopedPtr otherGeom( asGeos( &point, mPrecision ) );
  if ( !otherGeom )
  {
    return -1;
  }

  double distance = -1;
  try
  {
    distance = GEOSProject_r( geosinit.ctxt, mGeos, otherGeom.get() );
  }
  catch ( GEOSException &e )
  {
    if ( errorMsg )
    {
      *errorMsg = e.what();
    }
    return -1;
  }

  return distance;
}


/** Extract coordinates of linestring's endpoints. Returns false on error. */
static bool _linestringEndpoints( const GEOSGeometry* linestring, double& x1, double& y1, double& x2, double& y2 )
{
  const GEOSCoordSequence* coordSeq = GEOSGeom_getCoordSeq_r( geosinit.ctxt, linestring );
  if ( !coordSeq )
    return false;

  unsigned int coordSeqSize;
  if ( GEOSCoordSeq_getSize_r( geosinit.ctxt, coordSeq, &coordSeqSize ) == 0 )
    return false;

  if ( coordSeqSize < 2 )
    return false;

  GEOSCoordSeq_getX_r( geosinit.ctxt, coordSeq, 0, &x1 );
  GEOSCoordSeq_getY_r( geosinit.ctxt, coordSeq, 0, &y1 );
  GEOSCoordSeq_getX_r( geosinit.ctxt, coordSeq, coordSeqSize - 1, &x2 );
  GEOSCoordSeq_getY_r( geosinit.ctxt, coordSeq, coordSeqSize - 1, &y2 );
  return true;
}


/** Merge two linestrings if they meet at the given intersection point, return new geometry or null on error. */
static GEOSGeometry* _mergeLinestrings( const GEOSGeometry* line1, const GEOSGeometry* line2, const QgsPoint& interesectionPoint )
{
  double x1, y1, x2, y2;
  if ( !_linestringEndpoints( line1, x1, y1, x2, y2 ) )
    return nullptr;

  double rx1, ry1, rx2, ry2;
  if ( !_linestringEndpoints( line2, rx1, ry1, rx2, ry2 ) )
    return nullptr;

  bool interesectionAtOrigLineEndpoint =
    ( interesectionPoint.x() == x1 && interesectionPoint.y() == y1 ) ||
    ( interesectionPoint.x() == x2 && interesectionPoint.y() == y2 );
  bool interesectionAtReshapeLineEndpoint =
    ( interesectionPoint.x() == rx1 && interesectionPoint.y() == ry1 ) ||
    ( interesectionPoint.x() == rx2 && interesectionPoint.y() == ry2 );

  // the intersection must be at the begin/end of both lines
  if ( interesectionAtOrigLineEndpoint && interesectionAtReshapeLineEndpoint )
  {
    GEOSGeometry* g1 = GEOSGeom_clone_r( geosinit.ctxt, line1 );
    GEOSGeometry* g2 = GEOSGeom_clone_r( geosinit.ctxt, line2 );
    GEOSGeometry* geoms[2] = { g1, g2 };
    GEOSGeometry* multiGeom = GEOSGeom_createCollection_r( geosinit.ctxt, GEOS_MULTILINESTRING, geoms, 2 );
    GEOSGeometry* res = GEOSLineMerge_r( geosinit.ctxt, multiGeom );
    GEOSGeom_destroy_r( geosinit.ctxt, multiGeom );
    return res;
  }
  else
    return nullptr;
}


GEOSGeometry* QgsGeos::reshapeLine( const GEOSGeometry* line, const GEOSGeometry* reshapeLineGeos , double precision )
{
  if ( !line || !reshapeLineGeos )
    return nullptr;

  bool atLeastTwoIntersections = false;
  bool oneIntersection = false;
  QgsPoint oneIntersectionPoint;

  try
  {
    //make sure there are at least two intersection between line and reshape geometry
    GEOSGeometry* intersectGeom = GEOSIntersection_r( geosinit.ctxt, line, reshapeLineGeos );
    if ( intersectGeom )
    {
      atLeastTwoIntersections = ( GEOSGeomTypeId_r( geosinit.ctxt, intersectGeom ) == GEOS_MULTIPOINT
                                  && GEOSGetNumGeometries_r( geosinit.ctxt, intersectGeom ) > 1 );
      // one point is enough when extending line at its endpoint
      if ( GEOSGeomTypeId_r( geosinit.ctxt, intersectGeom ) == GEOS_POINT )
      {
        const GEOSCoordSequence* intersectionCoordSeq = GEOSGeom_getCoordSeq_r( geosinit.ctxt, intersectGeom );
        double xi, yi;
        GEOSCoordSeq_getX_r( geosinit.ctxt, intersectionCoordSeq, 0, &xi );
        GEOSCoordSeq_getY_r( geosinit.ctxt, intersectionCoordSeq, 0, &yi );
        oneIntersection = true;
        oneIntersectionPoint = QgsPoint( xi, yi );
      }
      GEOSGeom_destroy_r( geosinit.ctxt, intersectGeom );
    }
  }
  catch ( GEOSException &e )
  {
    QgsMessageLog::logMessage( QObject::tr( "Exception: %1" ).arg( e.what() ), QObject::tr( "GEOS" ) );
    atLeastTwoIntersections = false;
  }

  // special case when extending line at its endpoint
  if ( oneIntersection )
    return _mergeLinestrings( line, reshapeLineGeos, oneIntersectionPoint );

  if ( !atLeastTwoIntersections )
    return nullptr;

  //begin and end point of original line
  double x1, y1, x2, y2;
  if ( !_linestringEndpoints( line, x1, y1, x2, y2 ) )
    return nullptr;

  QgsPointV2 beginPoint( x1, y1 );
  GEOSGeometry* beginLineVertex = createGeosPoint( &beginPoint, 2, precision );
  QgsPointV2 endPoint( x2, y2 );
  GEOSGeometry* endLineVertex = createGeosPoint( &endPoint, 2, precision );

  bool isRing = false;
  if ( GEOSGeomTypeId_r( geosinit.ctxt, line ) == GEOS_LINEARRING
       || GEOSEquals_r( geosinit.ctxt, beginLineVertex, endLineVertex ) == 1 )
    isRing = true;

  //node line and reshape line
  GEOSGeometry* nodedGeometry = nodeGeometries( reshapeLineGeos, line );
  if ( !nodedGeometry )
  {
    GEOSGeom_destroy_r( geosinit.ctxt, beginLineVertex );
    GEOSGeom_destroy_r( geosinit.ctxt, endLineVertex );
    return nullptr;
  }

  //and merge them together
  GEOSGeometry *mergedLines = GEOSLineMerge_r( geosinit.ctxt, nodedGeometry );
  GEOSGeom_destroy_r( geosinit.ctxt, nodedGeometry );
  if ( !mergedLines )
  {
    GEOSGeom_destroy_r( geosinit.ctxt, beginLineVertex );
    GEOSGeom_destroy_r( geosinit.ctxt, endLineVertex );
    return nullptr;
  }

  int numMergedLines = GEOSGetNumGeometries_r( geosinit.ctxt, mergedLines );
  if ( numMergedLines < 2 ) //some special cases. Normally it is >2
  {
    GEOSGeom_destroy_r( geosinit.ctxt, beginLineVertex );
    GEOSGeom_destroy_r( geosinit.ctxt, endLineVertex );
    if ( numMergedLines == 1 ) //reshape line is from begin to endpoint. So we keep the reshapeline
      return GEOSGeom_clone_r( geosinit.ctxt, reshapeLineGeos );
    else
      return nullptr;
  }

  QList<GEOSGeometry*> resultLineParts; //collection with the line segments that will be contained in result
  QList<GEOSGeometry*> probableParts; //parts where we can decide on inclusion only after going through all the candidates

  for ( int i = 0; i < numMergedLines; ++i )
  {
    const GEOSGeometry* currentGeom;

    currentGeom = GEOSGetGeometryN_r( geosinit.ctxt, mergedLines, i );
    const GEOSCoordSequence* currentCoordSeq = GEOSGeom_getCoordSeq_r( geosinit.ctxt, currentGeom );
    unsigned int currentCoordSeqSize;
    GEOSCoordSeq_getSize_r( geosinit.ctxt, currentCoordSeq, &currentCoordSeqSize );
    if ( currentCoordSeqSize < 2 )
      continue;

    //get the two endpoints of the current line merge result
    double xBegin, xEnd, yBegin, yEnd;
    GEOSCoordSeq_getX_r( geosinit.ctxt, currentCoordSeq, 0, &xBegin );
    GEOSCoordSeq_getY_r( geosinit.ctxt, currentCoordSeq, 0, &yBegin );
    GEOSCoordSeq_getX_r( geosinit.ctxt, currentCoordSeq, currentCoordSeqSize - 1, &xEnd );
    GEOSCoordSeq_getY_r( geosinit.ctxt, currentCoordSeq, currentCoordSeqSize - 1, &yEnd );
    QgsPointV2 beginPoint( xBegin, yBegin );
    GEOSGeometry* beginCurrentGeomVertex = createGeosPoint( &beginPoint, 2, precision );
    QgsPointV2 endPoint( xEnd, yEnd );
    GEOSGeometry* endCurrentGeomVertex = createGeosPoint( &endPoint, 2, precision );

    //check how many endpoints of the line merge result are on the (original) line
    int nEndpointsOnOriginalLine = 0;
    if ( pointContainedInLine( beginCurrentGeomVertex, line ) == 1 )
      nEndpointsOnOriginalLine += 1;

    if ( pointContainedInLine( endCurrentGeomVertex, line ) == 1 )
      nEndpointsOnOriginalLine += 1;

    //check how many endpoints equal the endpoints of the original line
    int nEndpointsSameAsOriginalLine = 0;
    if ( GEOSEquals_r( geosinit.ctxt, beginCurrentGeomVertex, beginLineVertex ) == 1
         || GEOSEquals_r( geosinit.ctxt, beginCurrentGeomVertex, endLineVertex ) == 1 )
      nEndpointsSameAsOriginalLine += 1;

    if ( GEOSEquals_r( geosinit.ctxt, endCurrentGeomVertex, beginLineVertex ) == 1
         || GEOSEquals_r( geosinit.ctxt, endCurrentGeomVertex, endLineVertex ) == 1 )
      nEndpointsSameAsOriginalLine += 1;

    //check if the current geometry overlaps the original geometry (GEOSOverlap does not seem to work with linestrings)
    bool currentGeomOverlapsOriginalGeom = false;
    bool currentGeomOverlapsReshapeLine = false;
    if ( lineContainedInLine( currentGeom, line ) == 1 )
      currentGeomOverlapsOriginalGeom = true;

    if ( lineContainedInLine( currentGeom, reshapeLineGeos ) == 1 )
      currentGeomOverlapsReshapeLine = true;

    //logic to decide if this part belongs to the result
    if ( !isRing && nEndpointsSameAsOriginalLine == 1 && nEndpointsOnOriginalLine == 2 && currentGeomOverlapsOriginalGeom )
    {
      resultLineParts.push_back( GEOSGeom_clone_r( geosinit.ctxt, currentGeom ) );
    }
    //for closed rings, we take one segment from the candidate list
    else if ( isRing && nEndpointsOnOriginalLine == 2 && currentGeomOverlapsOriginalGeom )
    {
      probableParts.push_back( GEOSGeom_clone_r( geosinit.ctxt, currentGeom ) );
    }
    else if ( nEndpointsOnOriginalLine == 2 && !currentGeomOverlapsOriginalGeom )
    {
      resultLineParts.push_back( GEOSGeom_clone_r( geosinit.ctxt, currentGeom ) );
    }
    else if ( nEndpointsSameAsOriginalLine == 2 && !currentGeomOverlapsOriginalGeom )
    {
      resultLineParts.push_back( GEOSGeom_clone_r( geosinit.ctxt, currentGeom ) );
    }
    else if ( currentGeomOverlapsOriginalGeom && currentGeomOverlapsReshapeLine )
    {
      resultLineParts.push_back( GEOSGeom_clone_r( geosinit.ctxt, currentGeom ) );
    }

    GEOSGeom_destroy_r( geosinit.ctxt, beginCurrentGeomVertex );
    GEOSGeom_destroy_r( geosinit.ctxt, endCurrentGeomVertex );
  }

  //add the longest segment from the probable list for rings (only used for polygon rings)
  if ( isRing && !probableParts.isEmpty() )
  {
    GEOSGeometry* maxGeom = nullptr; //the longest geometry in the probabla list
    GEOSGeometry* currentGeom = nullptr;
    double maxLength = -std::numeric_limits<double>::max();
    double currentLength = 0;
    for ( int i = 0; i < probableParts.size(); ++i )
    {
      currentGeom = probableParts.at( i );
      GEOSLength_r( geosinit.ctxt, currentGeom, &currentLength );
      if ( currentLength > maxLength )
      {
        maxLength = currentLength;
        GEOSGeom_destroy_r( geosinit.ctxt, maxGeom );
        maxGeom = currentGeom;
      }
      else
      {
        GEOSGeom_destroy_r( geosinit.ctxt, currentGeom );
      }
    }
    resultLineParts.push_back( maxGeom );
  }

  GEOSGeom_destroy_r( geosinit.ctxt, beginLineVertex );
  GEOSGeom_destroy_r( geosinit.ctxt, endLineVertex );
  GEOSGeom_destroy_r( geosinit.ctxt, mergedLines );

  GEOSGeometry* result = nullptr;
  if ( resultLineParts.size() < 1 )
    return nullptr;

  if ( resultLineParts.size() == 1 ) //the whole result was reshaped
  {
    result = resultLineParts[0];
  }
  else //>1
  {
    GEOSGeometry **lineArray = new GEOSGeometry*[resultLineParts.size()];
    for ( int i = 0; i < resultLineParts.size(); ++i )
    {
      lineArray[i] = resultLineParts[i];
    }

    //create multiline from resultLineParts
    GEOSGeometry* multiLineGeom = GEOSGeom_createCollection_r( geosinit.ctxt, GEOS_MULTILINESTRING, lineArray, resultLineParts.size() );
    delete [] lineArray;

    //then do a linemerge with the newly combined partstrings
    result = GEOSLineMerge_r( geosinit.ctxt, multiLineGeom );
    GEOSGeom_destroy_r( geosinit.ctxt, multiLineGeom );
  }

  //now test if the result is a linestring. Otherwise something went wrong
  if ( GEOSGeomTypeId_r( geosinit.ctxt, result ) != GEOS_LINESTRING )
  {
    GEOSGeom_destroy_r( geosinit.ctxt, result );
    return nullptr;
  }

  return result;
}

GEOSGeometry* QgsGeos::reshapePolygon( const GEOSGeometry* polygon, const GEOSGeometry* reshapeLineGeos, double precision )
{
  //go through outer shell and all inner rings and check if there is exactly one intersection of a ring and the reshape line
  int nIntersections = 0;
  int lastIntersectingRing = -2;
  const GEOSGeometry* lastIntersectingGeom = nullptr;

  int nRings = GEOSGetNumInteriorRings_r( geosinit.ctxt, polygon );
  if ( nRings < 0 )
    return nullptr;

  //does outer ring intersect?
  const GEOSGeometry* outerRing = GEOSGetExteriorRing_r( geosinit.ctxt, polygon );
  if ( GEOSIntersects_r( geosinit.ctxt, outerRing, reshapeLineGeos ) == 1 )
  {
    ++nIntersections;
    lastIntersectingRing = -1;
    lastIntersectingGeom = outerRing;
  }

  //do inner rings intersect?
  const GEOSGeometry **innerRings = new const GEOSGeometry*[nRings];

  try
  {
    for ( int i = 0; i < nRings; ++i )
    {
      innerRings[i] = GEOSGetInteriorRingN_r( geosinit.ctxt, polygon, i );
      if ( GEOSIntersects_r( geosinit.ctxt, innerRings[i], reshapeLineGeos ) == 1 )
      {
        ++nIntersections;
        lastIntersectingRing = i;
        lastIntersectingGeom = innerRings[i];
      }
    }
  }
  catch ( GEOSException &e )
  {
    QgsMessageLog::logMessage( QObject::tr( "Exception: %1" ).arg( e.what() ), QObject::tr( "GEOS" ) );
    nIntersections = 0;
  }

  if ( nIntersections != 1 ) //reshape line is only allowed to intersect one ring
  {
    delete [] innerRings;
    return nullptr;
  }

  //we have one intersecting ring, let's try to reshape it
  GEOSGeometry* reshapeResult = reshapeLine( lastIntersectingGeom, reshapeLineGeos, precision );
  if ( !reshapeResult )
  {
    delete [] innerRings;
    return nullptr;
  }

  //if reshaping took place, we need to reassemble the polygon and its rings
  GEOSGeometry* newRing = nullptr;
  const GEOSCoordSequence* reshapeSequence = GEOSGeom_getCoordSeq_r( geosinit.ctxt, reshapeResult );
  GEOSCoordSequence* newCoordSequence = GEOSCoordSeq_clone_r( geosinit.ctxt, reshapeSequence );

  GEOSGeom_destroy_r( geosinit.ctxt, reshapeResult );

  newRing = GEOSGeom_createLinearRing_r( geosinit.ctxt, newCoordSequence );
  if ( !newRing )
  {
    delete [] innerRings;
    return nullptr;
  }

  GEOSGeometry* newOuterRing = nullptr;
  if ( lastIntersectingRing == -1 )
    newOuterRing = newRing;
  else
    newOuterRing = GEOSGeom_clone_r( geosinit.ctxt, outerRing );

  //check if all the rings are still inside the outer boundary
  QList<GEOSGeometry*> ringList;
  if ( nRings > 0 )
  {
    GEOSGeometry* outerRingPoly = GEOSGeom_createPolygon_r( geosinit.ctxt, GEOSGeom_clone_r( geosinit.ctxt, newOuterRing ), nullptr, 0 );
    if ( outerRingPoly )
    {
      GEOSGeometry* currentRing = nullptr;
      for ( int i = 0; i < nRings; ++i )
      {
        if ( lastIntersectingRing == i )
          currentRing = newRing;
        else
          currentRing = GEOSGeom_clone_r( geosinit.ctxt, innerRings[i] );

        //possibly a ring is no longer contained in the result polygon after reshape
        if ( GEOSContains_r( geosinit.ctxt, outerRingPoly, currentRing ) == 1 )
          ringList.push_back( currentRing );
        else
          GEOSGeom_destroy_r( geosinit.ctxt, currentRing );
      }
    }
    GEOSGeom_destroy_r( geosinit.ctxt, outerRingPoly );
  }

  GEOSGeometry** newInnerRings = new GEOSGeometry*[ringList.size()];
  for ( int i = 0; i < ringList.size(); ++i )
    newInnerRings[i] = ringList.at( i );

  delete [] innerRings;

  GEOSGeometry* reshapedPolygon = GEOSGeom_createPolygon_r( geosinit.ctxt, newOuterRing, newInnerRings, ringList.size() );
  delete[] newInnerRings;

  return reshapedPolygon;
}

int QgsGeos::lineContainedInLine( const GEOSGeometry* line1, const GEOSGeometry* line2 )
{
  if ( !line1 || !line2 )
  {
    return -1;
  }

  double bufferDistance = pow( 10.0L, geomDigits( line2 ) - 11 );

  GEOSGeometry* bufferGeom = GEOSBuffer_r( geosinit.ctxt, line2, bufferDistance, DEFAULT_QUADRANT_SEGMENTS );
  if ( !bufferGeom )
    return -2;

  GEOSGeometry* intersectionGeom = GEOSIntersection_r( geosinit.ctxt, bufferGeom, line1 );

  //compare ratio between line1Length and intersectGeomLength (usually close to 1 if line1 is contained in line2)
  double intersectGeomLength;
  double line1Length;

  GEOSLength_r( geosinit.ctxt, intersectionGeom, &intersectGeomLength );
  GEOSLength_r( geosinit.ctxt, line1, &line1Length );

  GEOSGeom_destroy_r( geosinit.ctxt, bufferGeom );
  GEOSGeom_destroy_r( geosinit.ctxt, intersectionGeom );

  double intersectRatio = line1Length / intersectGeomLength;
  if ( intersectRatio > 0.9 && intersectRatio < 1.1 )
    return 1;

  return 0;
}

int QgsGeos::pointContainedInLine( const GEOSGeometry* point, const GEOSGeometry* line )
{
  if ( !point || !line )
    return -1;

  double bufferDistance = pow( 10.0L, geomDigits( line ) - 11 );

  GEOSGeometry* lineBuffer = GEOSBuffer_r( geosinit.ctxt, line, bufferDistance, 8 );
  if ( !lineBuffer )
    return -2;

  bool contained = false;
  if ( GEOSContains_r( geosinit.ctxt, lineBuffer, point ) == 1 )
    contained = true;

  GEOSGeom_destroy_r( geosinit.ctxt, lineBuffer );
  return contained;
}

int QgsGeos::geomDigits( const GEOSGeometry* geom )
{
  GEOSGeomScopedPtr bbox( GEOSEnvelope_r( geosinit.ctxt, geom ) );
  if ( !bbox.get() )
    return -1;

  const GEOSGeometry* bBoxRing = GEOSGetExteriorRing_r( geosinit.ctxt, bbox.get() );
  if ( !bBoxRing )
    return -1;

  const GEOSCoordSequence* bBoxCoordSeq = GEOSGeom_getCoordSeq_r( geosinit.ctxt, bBoxRing );

  if ( !bBoxCoordSeq )
    return -1;

  unsigned int nCoords = 0;
  if ( !GEOSCoordSeq_getSize_r( geosinit.ctxt, bBoxCoordSeq, &nCoords ) )
    return -1;

  int maxDigits = -1;
  for ( unsigned int i = 0; i < nCoords - 1; ++i )
  {
    double t;
    GEOSCoordSeq_getX_r( geosinit.ctxt, bBoxCoordSeq, i, &t );

    int digits;
    digits = ceil( log10( fabs( t ) ) );
    if ( digits > maxDigits )
      maxDigits = digits;

    GEOSCoordSeq_getY_r( geosinit.ctxt, bBoxCoordSeq, i, &t );
    digits = ceil( log10( fabs( t ) ) );
    if ( digits > maxDigits )
      maxDigits = digits;
  }

  return maxDigits;
}

GEOSContextHandle_t QgsGeos::getGEOSHandler()
{
  return geosinit.ctxt;
}
