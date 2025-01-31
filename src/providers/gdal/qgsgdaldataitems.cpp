/***************************************************************************
    qgsgdaldataitems.cpp
    ---------------------
    begin                : October 2011
    copyright            : (C) 2011 by Martin Dobias
    email                : wonder dot sk at gmail dot com
 ***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/
#include "qgsgdaldataitems.h"
#include "qgsgdalprovider.h"
#include "qgslogger.h"

#include <QFileInfo>
#include <QSettings>

// defined in qgsgdalprovider.cpp
void buildSupportedRasterFileFilterAndExtensions( QString & theFileFiltersString, QStringList & theExtensions, QStringList & theWildcards );


QgsGdalLayerItem::QgsGdalLayerItem( QgsDataItem* parent,
                                    QString name, QString path, QString uri,
                                    QStringList *theSublayers )
    : QgsLayerItem( parent, name, path, uri, QgsLayerItem::Raster, "gdal" )
{
  mToolTip = uri;
  // save sublayers for subsequent access
  // if there are sublayers, set populated=false so item can be populated on demand
  if ( theSublayers && !theSublayers->isEmpty() )
  {
    sublayers = *theSublayers;
    setState( NotPopulated );
  }
  else
    setState( Populated );

  GDALAllRegister();
  GDALDatasetH hDS = GDALOpen( TO8F( mPath ), GA_Update );

  if ( hDS )
  {
    mCapabilities |= SetCrs;
    GDALClose( hDS );
  }
}

QgsGdalLayerItem::~QgsGdalLayerItem()
{
}

Q_NOWARN_DEPRECATED_PUSH
QgsLayerItem::Capability QgsGdalLayerItem::capabilities()
{
  return mCapabilities & SetCrs ? SetCrs : NoCapabilities;
}
Q_NOWARN_DEPRECATED_POP

bool QgsGdalLayerItem::setCrs( QgsCoordinateReferenceSystem crs )
{
  GDALDatasetH hDS = GDALOpen( TO8F( mPath ), GA_Update );
  if ( !hDS )
    return false;

  QString wkt = crs.toWkt();
  if ( GDALSetProjection( hDS, wkt.toLocal8Bit().data() ) != CE_None )
  {
    GDALClose( hDS );
    QgsDebugMsg( "Could not set CRS" );
    return false;
  }

  GDALClose( hDS );
  return true;
}

QVector<QgsDataItem*> QgsGdalLayerItem::createChildren()
{
  QgsDebugMsg( "Entered, path=" + path() );
  QVector<QgsDataItem*> children;

  // get children from sublayers
  if ( !sublayers.isEmpty() )
  {
    QgsDataItem * childItem = nullptr;
    QgsDebugMsg( QString( "got %1 sublayers" ).arg( sublayers.count() ) );
    for ( int i = 0; i < sublayers.count(); i++ )
    {
      QString name = sublayers[i];
      // if netcdf/hdf use all text after filename
      // for hdf4 it would be best to get description, because the subdataset_index is not very practical
      if ( name.startsWith( "netcdf", Qt::CaseInsensitive ) ||
           name.startsWith( "hdf", Qt::CaseInsensitive ) )
        name = name.mid( name.indexOf( mPath ) + mPath.length() + 1 );
      else
      {
        // remove driver name and file name
        name.remove( name.split( ':' )[0] );
        name.remove( mPath );
      }
      // remove any : or " left over
      if ( name.startsWith( ':' ) ) name.remove( 0, 1 );
      if ( name.startsWith( '\"' ) ) name.remove( 0, 1 );
      if ( name.endsWith( ':' ) ) name.chop( 1 );
      if ( name.endsWith( '\"' ) ) name.chop( 1 );

      childItem = new QgsGdalLayerItem( this, name, sublayers[i], sublayers[i] );
      if ( childItem )
        this->addChildItem( childItem );
    }
  }

  return children;
}

QString QgsGdalLayerItem::layerName() const
{
  QFileInfo info( name() );
  if ( info.suffix() == "gz" )
    return info.baseName();
  else
    return info.completeBaseName();
}

// ---------------------------------------------------------------------------

static QString filterString;
static QStringList extensions = QStringList();
static QStringList wildcards = QStringList();
static QMutex gBuildingFilters;

QGISEXTERN int dataCapabilities()
{
  return QgsDataProvider::File | QgsDataProvider::Dir | QgsDataProvider::Net;
}

QGISEXTERN QgsDataItem * dataItem( QString thePath, QgsDataItem* parentItem )
{
  if ( thePath.isEmpty() )
    return nullptr;

  QgsDebugMsgLevel( "thePath = " + thePath, 2 );

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

  // get supported extensions
  if ( extensions.isEmpty() )
  {
    // this code may be executed by more threads at once!
    // use a mutex to make sure this does not happen (so there's no crash on start)
    QMutexLocker locker( &gBuildingFilters );
    if ( extensions.isEmpty() )
    {
      buildSupportedRasterFileFilterAndExtensions( filterString, extensions, wildcards );
      QgsDebugMsgLevel( "extensions: " + extensions.join( " " ), 2 );
      QgsDebugMsgLevel( "wildcards: " + wildcards.join( " " ), 2 );
    }
  }

  // skip *.aux.xml files (GDAL auxilary metadata files),
  // *.shp.xml files (ESRI metadata) and *.tif.xml files (TIFF metadata)
  // unless that extension is in the list (*.xml might be though)
  if ( thePath.endsWith( ".aux.xml", Qt::CaseInsensitive ) &&
       !extensions.contains( "aux.xml" ) )
    return nullptr;
  if ( thePath.endsWith( ".shp.xml", Qt::CaseInsensitive ) &&
       !extensions.contains( "shp.xml" ) )
    return nullptr;
  if ( thePath.endsWith( ".tif.xml", Qt::CaseInsensitive ) &&
       !extensions.contains( "tif.xml" ) )
    return nullptr;

  // Filter files by extension
  if ( !extensions.contains( suffix ) )
  {
    bool matches = false;
    Q_FOREACH ( const QString& wildcard, wildcards )
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
    // if this is a VRT file make sure it is raster VRT to avoid duplicates
    if ( suffix == "vrt" )
    {
      // do not print errors, but write to debug
      CPLPushErrorHandler( CPLQuietErrorHandler );
      CPLErrorReset();
      GDALDriverH hDriver = GDALIdentifyDriver( TO8F( thePath ), nullptr );
      CPLPopErrorHandler();
      if ( !hDriver || GDALGetDriverShortName( hDriver ) == QString( "OGR_VRT" ) )
      {
        QgsDebugMsgLevel( "Skipping VRT file because root is not a GDAL VRT", 2 );
        return nullptr;
      }
    }
    // add the item
    QStringList sublayers;
    QgsDebugMsgLevel( QString( "adding item name=%1 thePath=%2" ).arg( name, thePath ), 2 );
    QgsLayerItem * item = new QgsGdalLayerItem( parentItem, name, thePath, thePath, &sublayers );
    if ( item )
      return item;
  }

  // test that file is valid with GDAL
  GDALAllRegister();
  // do not print errors, but write to debug
  CPLPushErrorHandler( CPLQuietErrorHandler );
  CPLErrorReset();
  GDALDatasetH hDS = GDALOpen( TO8F( thePath ), GA_ReadOnly );
  CPLPopErrorHandler();

  if ( ! hDS )
  {
    QgsDebugMsg( QString( "GDALOpen error # %1 : %2 " ).arg( CPLGetLastErrorNo() ).arg( CPLGetLastErrorMsg() ) );
    return nullptr;
  }

  QStringList sublayers = QgsGdalProvider::subLayers( hDS );

  GDALClose( hDS );

  QgsDebugMsgLevel( "GdalDataset opened " + thePath, 2 );

  QgsLayerItem * item = new QgsGdalLayerItem( parentItem, name, thePath, thePath,
      &sublayers );

  return item;
}

