/***************************************************************************
                             qgsogrdataitems.cpp
                             -------------------
    begin                : 2011-04-01
    copyright            : (C) 2011 Radim Blazek
    email                : radim dot blazek at gmail dot com
 ***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include "qgsogrdataitems.h"

#include "qgslogger.h"
#include "qgsmessagelog.h"

#include <QFileInfo>
#include <QTextStream>
#include <QSettings>

#include <ogr_srs_api.h>
#include <cpl_error.h>
#include <cpl_conv.h>

// these are defined in qgsogrprovider.cpp
QGISEXTERN QStringList fileExtensions();
QGISEXTERN QStringList wildcards();


QgsOgrLayerItem::QgsOgrLayerItem( QgsDataItem* parent,
                                  QString name, QString path, QString uri, LayerType layerType )
    : QgsLayerItem( parent, name, path, uri, layerType, "ogr" )
{
  mToolTip = uri;
  setState( Populated ); // children are not expected

  OGRRegisterAll();
  OGRSFDriverH hDriver;
  OGRDataSourceH hDataSource = QgsOgrProviderUtils::OGROpenWrapper( TO8F( mPath ), true, &hDriver );

  if ( hDataSource )
  {
    QString driverName = OGR_Dr_GetName( hDriver );
    OGR_DS_Destroy( hDataSource );

    if ( driverName == "ESRI Shapefile" )
      mCapabilities |= SetCrs;

    // It it is impossible to assign a crs to an existing layer
    // No OGR_L_SetSpatialRef : http://trac.osgeo.org/gdal/ticket/4032
  }
}

QgsOgrLayerItem::~QgsOgrLayerItem()
{
}

Q_NOWARN_DEPRECATED_PUSH
QgsLayerItem::Capability QgsOgrLayerItem::capabilities()
{
  return mCapabilities & SetCrs ? SetCrs : NoCapabilities;
}
Q_NOWARN_DEPRECATED_POP

bool QgsOgrLayerItem::setCrs( QgsCoordinateReferenceSystem crs )
{
  if ( !( mCapabilities & SetCrs ) )
    return false;

  QString layerName = mPath.left( mPath.indexOf( ".shp", Qt::CaseInsensitive ) );
  QString wkt = crs.toWkt();

  // save ordinary .prj file
  OGRSpatialReferenceH hSRS = OSRNewSpatialReference( wkt.toLocal8Bit().data() );
  OSRSetAxisMappingStrategy(hSRS, OAMS_TRADITIONAL_GIS_ORDER);
  OSRMorphToESRI( hSRS ); // this is the important stuff for shapefile .prj
  char* pszOutWkt = nullptr;
  OSRExportToWkt( hSRS, &pszOutWkt );
  QFile prjFile( layerName + ".prj" );
  if ( prjFile.open( QIODevice::WriteOnly ) )
  {
    QTextStream prjStream( &prjFile );
    prjStream << pszOutWkt << endl;
    prjFile.close();
  }
  else
  {
    QgsMessageLog::logMessage( tr( "Couldn't open file %1.prj" ).arg( layerName ), tr( "OGR" ) );
    return false;
  }
  OSRDestroySpatialReference( hSRS );
  CPLFree( pszOutWkt );

  // save qgis-specific .qpj file (maybe because of better wkt compatibility?)
  QFile qpjFile( layerName + ".qpj" );
  if ( qpjFile.open( QIODevice::WriteOnly ) )
  {
    QTextStream qpjStream( &qpjFile );
    qpjStream << wkt.toLocal8Bit().data() << endl;
    qpjFile.close();
  }
  else
  {
    QgsMessageLog::logMessage( tr( "Couldn't open file %1.qpj" ).arg( layerName ), tr( "OGR" ) );
    return false;
  }

  return true;
}

QString QgsOgrLayerItem::layerName() const
{
  QFileInfo info( name() );
  if ( info.suffix() == "gz" )
    return info.baseName();
  else
    return info.completeBaseName();
}

// -------

static QgsOgrLayerItem* dataItemForLayer( QgsDataItem* parentItem, QString name, QString path, OGRDataSourceH hDataSource, int layerId )
{
  OGRLayerH hLayer = OGR_DS_GetLayer( hDataSource, layerId );
  OGRFeatureDefnH hDef = OGR_L_GetLayerDefn( hLayer );

  QgsLayerItem::LayerType layerType = QgsLayerItem::Vector;
  OGRwkbGeometryType ogrType = QgsOgrProvider::getOgrGeomType( hLayer );
  switch ( ogrType )
  {
    case wkbUnknown:
    case wkbGeometryCollection:
      break;
    case wkbNone:
      layerType = QgsLayerItem::TableLayer;
      break;
    case wkbPoint:
    case wkbMultiPoint:
    case wkbPoint25D:
    case wkbMultiPoint25D:
      layerType = QgsLayerItem::Point;
      break;
    case wkbLineString:
    case wkbMultiLineString:
    case wkbLineString25D:
    case wkbMultiLineString25D:
      layerType = QgsLayerItem::Line;
      break;
    case wkbPolygon:
    case wkbMultiPolygon:
    case wkbPolygon25D:
    case wkbMultiPolygon25D:
      layerType = QgsLayerItem::Polygon;
      break;
    default:
      break;
  }

  QgsDebugMsgLevel( QString( "ogrType = %1 layertype = %2" ).arg( ogrType ).arg( layerType ), 2 );

  QString layerUri = path;

  if ( name.isEmpty() )
  {
    // we are in a collection
    name = FROM8( OGR_FD_GetName( hDef ) );
    QgsDebugMsg( "OGR layer name : " + name );

    layerUri += "|layerid=" + QString::number( layerId );

    path += '/' + name;
  }

  QgsDebugMsgLevel( "OGR layer uri : " + layerUri, 2 );

  return new QgsOgrLayerItem( parentItem, name, path, layerUri, layerType );
}

// ----

QgsOgrDataCollectionItem::QgsOgrDataCollectionItem( QgsDataItem* parent, QString name, QString path )
    : QgsDataCollectionItem( parent, name, path )
{
}

QgsOgrDataCollectionItem::~QgsOgrDataCollectionItem()
{
}

QVector<QgsDataItem*> QgsOgrDataCollectionItem::createChildren()
{
  QVector<QgsDataItem*> children;

  OGRSFDriverH hDriver;
  OGRDataSourceH hDataSource = QgsOgrProviderUtils::OGROpenWrapper( TO8F( mPath ), false, &hDriver );
  if ( !hDataSource )
    return children;
  int numLayers = OGR_DS_GetLayerCount( hDataSource );

  children.reserve( numLayers );
  for ( int i = 0; i < numLayers; ++i )
  {
    QgsOgrLayerItem* item = dataItemForLayer( this, QString(), mPath, hDataSource, i );
    children.append( item );
  }

  OGR_DS_Destroy( hDataSource );

  return children;
}

// ---------------------------------------------------------------------------

QGISEXTERN int dataCapabilities()
{
  return  QgsDataProvider::File | QgsDataProvider::Dir;
}

QGISEXTERN QgsDataItem * dataItem( QString thePath, QgsDataItem* parentItem )
{
  if ( thePath.isEmpty() )
    return nullptr;

  QgsDebugMsgLevel( "thePath: " + thePath, 2 );

  // zip settings + info
  QSettings settings;
  QString scanZipSetting = settings.value( "/qgis/scanZipInBrowser2", "basic" ).toString();
  QString vsiPrefix = QgsZipItem::vsiPrefix( thePath );
  bool is_vsizip = ( vsiPrefix == "/vsizip/" );
  bool is_vsigzip = ( vsiPrefix == "/vsigzip/" );
  bool is_vsitar = ( vsiPrefix == "/vsitar/" );

  // should we check ext. only?
  // check if scanItemsInBrowser2 == extension or parent dir in scanItemsFastScanUris
  // TODO - do this in dir item, but this requires a way to inform which extensions are supported by provider
  // maybe a callback function or in the provider registry?
  bool scanExtSetting = false;
  if (( settings.value( "/qgis/scanItemsInBrowser2",
                        "extension" ).toString() == "extension" ) ||
      ( parentItem && settings.value( "/qgis/scanItemsFastScanUris",
                                      QStringList() ).toStringList().contains( parentItem->path() ) ) ||
      (( is_vsizip || is_vsitar ) && parentItem && parentItem->parent() &&
       settings.value( "/qgis/scanItemsFastScanUris",
                       QStringList() ).toStringList().contains( parentItem->parent()->path() ) ) )
  {
    scanExtSetting = true;
  }

  // get suffix, removing .gz if present
  QString tmpPath = thePath; //path used for testing, not for layer creation
  if ( is_vsigzip )
    tmpPath.chop( 3 );
  QFileInfo info( tmpPath );
  QString suffix = info.suffix().toLower();
  // extract basename with extension
  info.setFile( thePath );
  QString name = info.fileName();

  QgsDebugMsgLevel( "thePath= " + thePath + " tmpPath= " + tmpPath + " name= " + name
                    + " suffix= " + suffix + " vsiPrefix= " + vsiPrefix, 3 );

  // allow only normal files or VSIFILE items to continue
  if ( !info.isFile() && vsiPrefix == "" )
    return nullptr;

  QStringList myExtensions = fileExtensions();

  // skip *.aux.xml files (GDAL auxilary metadata files),
  // *.shp.xml files (ESRI metadata) and *.tif.xml files (TIFF metadata)
  // unless that extension is in the list (*.xml might be though)
  if ( thePath.endsWith( ".aux.xml", Qt::CaseInsensitive ) &&
       !myExtensions.contains( "aux.xml" ) )
    return nullptr;
  if ( thePath.endsWith( ".shp.xml", Qt::CaseInsensitive ) &&
       !myExtensions.contains( "shp.xml" ) )
    return nullptr;
  if ( thePath.endsWith( ".tif.xml", Qt::CaseInsensitive ) &&
       !myExtensions.contains( "tif.xml" ) )
    return nullptr;

  // We have to filter by extensions, otherwise e.g. all Shapefile files are displayed
  // because OGR drive can open also .dbf, .shx.
  if ( myExtensions.indexOf( suffix ) < 0 )
  {
    bool matches = false;
    Q_FOREACH ( const QString& wildcard, wildcards() )
    {
      QRegExp rx( wildcard, Qt::CaseInsensitive, QRegExp::Wildcard );
      if ( rx.exactMatch( info.fileName() ) )
      {
        matches = true;
        break;
      }
    }
    if ( !matches )
      return nullptr;
  }

  // .dbf should probably appear if .shp is not present
  if ( suffix == "dbf" )
  {
    QString pathShp = thePath.left( thePath.count() - 4 ) + ".shp";
    if ( QFileInfo( pathShp ).exists() )
      return nullptr;
  }

  // fix vsifile path and name
  if ( vsiPrefix != "" )
  {
    // add vsiPrefix to path if needed
    if ( !thePath.startsWith( vsiPrefix ) )
      thePath = vsiPrefix + thePath;
    // if this is a /vsigzip/path_to_zip.zip/file_inside_zip remove the full path from the name
    // no need to change the name I believe
#if 0
    if (( is_vsizip || is_vsitar ) && ( thePath != vsiPrefix + parentItem->path() ) )
    {
      name = thePath;
      name = name.replace( vsiPrefix + parentItem->path() + '/', "" );
    }
#endif
  }

  // return item without testing if:
  // scanExtSetting
  // or zipfile and scan zip == "Basic scan"
  if ( scanExtSetting ||
       (( is_vsizip || is_vsitar ) && scanZipSetting == "basic" ) )
  {
    // if this is a VRT file make sure it is vector VRT to avoid duplicates
    if ( suffix == "vrt" )
    {
      OGRSFDriverH hDriver = OGRGetDriverByName( "VRT" );
      if ( hDriver )
      {
        // do not print errors, but write to debug
        CPLPushErrorHandler( CPLQuietErrorHandler );
        CPLErrorReset();
        OGRDataSourceH hDataSource = OGR_Dr_Open( hDriver, thePath.toLocal8Bit().constData(), 0 );
        CPLPopErrorHandler();
        if ( ! hDataSource )
        {
          QgsDebugMsgLevel( "Skipping VRT file because root is not a OGR VRT", 2 );
          return nullptr;
        }
        OGR_DS_Destroy( hDataSource );
      }
    }
    // add the item
    // TODO: how to handle collections?
    QgsLayerItem * item = new QgsOgrLayerItem( parentItem, name, thePath, thePath, QgsLayerItem::Vector );
    if ( item )
      return item;
  }

  // test that file is valid with OGR
  OGRRegisterAll();
  OGRSFDriverH hDriver;
  // do not print errors, but write to debug
  CPLPushErrorHandler( CPLQuietErrorHandler );
  CPLErrorReset();
  OGRDataSourceH hDataSource = QgsOgrProviderUtils::OGROpenWrapper( TO8F( thePath ), false, &hDriver );
  CPLPopErrorHandler();

  if ( ! hDataSource )
  {
    QgsDebugMsg( QString( "OGROpen error # %1 : %2 " ).arg( CPLGetLastErrorNo() ).arg( CPLGetLastErrorMsg() ) );
    return nullptr;
  }

  QgsDebugMsgLevel( QString( "OGR Driver : %1" ).arg( OGR_Dr_GetName( hDriver ) ), 2 );

  int numLayers = OGR_DS_GetLayerCount( hDataSource );

  QgsDataItem* item = nullptr;

  if ( numLayers == 1 )
  {
    QgsDebugMsgLevel( QString( "using name = %1" ).arg( name ), 2 );
    item = dataItemForLayer( parentItem, name, thePath, hDataSource, 0 );
  }
  else if ( numLayers > 1 )
  {
    QgsDebugMsgLevel( QString( "using name = %1" ).arg( name ), 2 );
    item = new QgsOgrDataCollectionItem( parentItem, name, thePath );
  }

  OGR_DS_Destroy( hDataSource );
  return item;
}
