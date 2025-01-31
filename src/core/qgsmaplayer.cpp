/***************************************************************************
                          qgsmaplayer.cpp  -  description
                             -------------------
    begin                : Fri Jun 28 2002
    copyright            : (C) 2002 by Gary E.Sherman
    email                : sherman at mrcc.com
***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/


#include <QDateTime>
#include <QDir>
#include <QDomDocument>
#include <QDomElement>
#include <QDomImplementation>
#include <QDomNode>
#include <QFile>
#include <QFileInfo>
#include <QSettings> // TODO: get rid of it [MD]
#include <QTextStream>
#include <QUrl>

#include <sqlite3.h>

#include "qgsapplication.h"
#include "qgscoordinatereferencesystem.h"
#include "qgsdatasourceuri.h"
#include "qgslogger.h"
#include "qgsauthmanager.h"
#include "qgsmaplayer.h"
#include "qgsmaplayerlegend.h"
#include "qgsmaplayerstylemanager.h"
#include "qgspluginlayer.h"
#include "qgspluginlayerregistry.h"
#include "qgsprojectfiletransform.h"
#include "qgsproject.h"
#include "qgsproviderregistry.h"
#include "qgsrasterlayer.h"
#include "qgsrectangle.h"
#include "qgsvectorlayer.h"
#include "qgsvectordataprovider.h"
#include "qgsmaplayerregistry.h"
#include "qgsxmlutils.h"


QgsMapLayer::QgsMapLayer( QgsMapLayer::LayerType type,
                          const QString& lyrname,
                          const QString& source )
    : mValid( false ) // assume the layer is invalid
    , mDataSource( source )
    , mLayerOrigName( lyrname ) // store the original name
    , mID( "" )
    , mLayerType( type )
    , mBlendMode( QPainter::CompositionMode_SourceOver ) // Default to normal blending
    , mLegend( nullptr )
    , mStyleManager( new QgsMapLayerStyleManager( this ) )
{
  // Set the display name = internal name
  mLayerName = capitaliseLayerName( mLayerOrigName );

  mShortName = "";
  //mShortName.replace( QRegExp( "[\\W]" ), "_" );

  // Generate the unique ID of this layer
  QDateTime dt = QDateTime::currentDateTime();
  mID = lyrname + dt.toString( "yyyyMMddhhmmsszzz" );
  // Tidy the ID up to avoid characters that may cause problems
  // elsewhere (e.g in some parts of XML). Replaces every non-word
  // character (word characters are the alphabet, numbers and
  // underscore) with an underscore.
  // Note that the first backslashe in the regular expression is
  // there for the compiler, so the pattern is actually \W
  mID.replace( QRegExp( "[\\W]" ), "_" );

  //set some generous  defaults for scale based visibility
  mMinScale = 0;
  mMaxScale = 100000000;
  mScaleBasedVisibility = false;

  connect( this, SIGNAL( nameChanged() ), this, SIGNAL( layerNameChanged() ) );

  connect( mStyleManager, SIGNAL( currentStyleChanged( QString ) ), this, SIGNAL( styleChanged() ) );
}

QgsMapLayer::~QgsMapLayer()
{
  delete mLegend;
  delete mStyleManager;
}

QgsMapLayer::LayerType QgsMapLayer::type() const
{
  return mLayerType;
}

/** Get this layer's unique ID */
QString QgsMapLayer::id() const
{
  return mID;
}

void QgsMapLayer::setLayerName( const QString & name )
{
  setName( name );
}

void QgsMapLayer::setName( const QString& name )
{
  QString newName = capitaliseLayerName( name );
  if ( name == mLayerOrigName && newName == mLayerName )
    return;

  mLayerOrigName = name; // store the new original name
  mLayerName = newName;

  emit nameChanged();
}

/** Read property of QString layerName. */
QString QgsMapLayer::name() const
{
  QgsDebugMsgLevel( "returning name '" + mLayerName + '\'', 4 );
  return mLayerName;
}

QString QgsMapLayer::publicSource() const
{
  // Redo this every time we're asked for it, as we don't know if
  // dataSource has changed.
  QString safeName = QgsDataSourceURI::removePassword( mDataSource );
  return safeName;
}

QString QgsMapLayer::source() const
{
  return mDataSource;
}

QgsRectangle QgsMapLayer::extent()
{
  return mExtent;
}

/** Write blend mode for layer */
void QgsMapLayer::setBlendMode( QPainter::CompositionMode blendMode )
{
  mBlendMode = blendMode;
  emit blendModeChanged( blendMode );
  emit styleChanged();
}

/** Read blend mode for layer */
QPainter::CompositionMode QgsMapLayer::blendMode() const
{
  return mBlendMode;
}

bool QgsMapLayer::draw( QgsRenderContext& rendererContext )
{
  Q_UNUSED( rendererContext );
  return false;
}

void QgsMapLayer::drawLabels( QgsRenderContext& rendererContext )
{
  Q_UNUSED( rendererContext );
}

bool QgsMapLayer::readLayerXML( const QDomElement& layerElement, const QString& relativeBasePath )
{
  bool layerError;

  QDomNode mnl;
  QDomElement mne;

  // read provider
  QString provider;
  mnl = layerElement.namedItem( "provider" );
  mne = mnl.toElement();
  provider = mne.text();

  // set data source
  mnl = layerElement.namedItem( "datasource" );
  mne = mnl.toElement();
  mDataSource = mne.text();

  // if the layer needs authentication, ensure the master password is set
  QRegExp rx( "authcfg=([a-z]|[A-Z]|[0-9]){7}" );
  if (( rx.indexIn( mDataSource ) != -1 )
      && !QgsAuthManager::instance()->setMasterPassword( true ) )
  {
    return false;
  }

  // TODO: this should go to providers
  // see also QgsProject::createEmbeddedLayer
  if ( provider == "spatialite" )
  {
    QgsDataSourceURI uri( mDataSource );
    uri.setDatabase( QgsProject::instance()->readPath( uri.database(), relativeBasePath ) );
    mDataSource = uri.uri();
  }
  else if ( provider == "ogr" )
  {
    QStringList theURIParts = mDataSource.split( '|' );
    theURIParts[0] = QgsProject::instance()->readPath( theURIParts[0], relativeBasePath );
    mDataSource = theURIParts.join( "|" );
  }
  else if ( provider == "gpx" )
  {
    QStringList theURIParts = mDataSource.split( '?' );
    theURIParts[0] = QgsProject::instance()->readPath( theURIParts[0], relativeBasePath );
    mDataSource = theURIParts.join( "?" );
  }
  else if ( provider == "delimitedtext" )
  {
    QUrl urlSource = QUrl::fromEncoded( mDataSource.toAscii() );

    if ( !mDataSource.startsWith( "file:" ) )
    {
      QUrl file = QUrl::fromLocalFile( mDataSource.left( mDataSource.indexOf( '?' ) ) );
      urlSource.setScheme( "file" );
      urlSource.setPath( file.path() );
    }

    QUrl urlDest = QUrl::fromLocalFile( QgsProject::instance()->readPath( urlSource.toLocalFile(), relativeBasePath ) );
    urlDest.setQueryItems( urlSource.queryItems() );
    mDataSource = QString::fromAscii( urlDest.toEncoded() );
  }
  else if ( provider == "wms" )
  {
    // >>> BACKWARD COMPATIBILITY < 1.9
    // For project file backward compatibility we must support old format:
    // 1. mode: <url>
    //    example: http://example.org/wms?
    // 2. mode: tiled=<width>;<height>;<resolution>;<resolution>...,ignoreUrl=GetMap;GetFeatureInfo,featureCount=<count>,username=<name>,password=<password>,url=<url>
    //    example: tiled=256;256;0.703;0.351,url=http://example.org/tilecache?
    //    example: featureCount=10,http://example.org/wms?
    //    example: ignoreUrl=GetMap;GetFeatureInfo,username=cimrman,password=jara,url=http://example.org/wms?
    // This is modified version of old QgsWmsProvider::parseUri
    // The new format has always params crs,format,layers,styles and that params
    // should not appear in old format url -> use them to identify version
    // XYZ tile layers do not need to contain crs,format params, but they have type=xyz
    if ( !mDataSource.contains( "type=" ) &&
         !mDataSource.contains( "crs=" ) && !mDataSource.contains( "format=" ) )
    {
      QgsDebugMsg( "Old WMS URI format detected -> converting to new format" );
      QgsDataSourceURI uri;
      if ( !mDataSource.startsWith( "http:" ) )
      {
        QStringList parts = mDataSource.split( ',' );
        QStringListIterator iter( parts );
        while ( iter.hasNext() )
        {
          QString item = iter.next();
          if ( item.startsWith( "username=" ) )
          {
            uri.setParam( "username", item.mid( 9 ) );
          }
          else if ( item.startsWith( "password=" ) )
          {
            uri.setParam( "password", item.mid( 9 ) );
          }
          else if ( item.startsWith( "tiled=" ) )
          {
            // in < 1.9 tiled= may apper in to variants:
            // tiled=width;height - non tiled mode, specifies max width and max height
            // tiled=width;height;resolutions-1;resolution2;... - tile mode

            QStringList params = item.mid( 6 ).split( ';' );

            if ( params.size() == 2 ) // non tiled mode
            {
              uri.setParam( "maxWidth", params.takeFirst() );
              uri.setParam( "maxHeight", params.takeFirst() );
            }
            else if ( params.size() > 2 ) // tiled mode
            {
              // resolutions are no more needed and size limit is not used for tiles
              // we have to tell to the provider however that it is tiled
              uri.setParam( "tileMatrixSet", "" );
            }
          }
          else if ( item.startsWith( "featureCount=" ) )
          {
            uri.setParam( "featureCount", item.mid( 13 ) );
          }
          else if ( item.startsWith( "url=" ) )
          {
            uri.setParam( "url", item.mid( 4 ) );
          }
          else if ( item.startsWith( "ignoreUrl=" ) )
          {
            uri.setParam( "ignoreUrl", item.mid( 10 ).split( ';' ) );
          }
        }
      }
      else
      {
        uri.setParam( "url", mDataSource );
      }
      mDataSource = uri.encodedUri();
      // At this point, the URI is obviously incomplete, we add additional params
      // in QgsRasterLayer::readXml
    }
    // <<< BACKWARD COMPATIBILITY < 1.9
  }
  else
  {
    bool handled = false;

    if ( provider == "gdal" )
    {
      if ( mDataSource.startsWith( "NETCDF:" ) )
      {
        // NETCDF:filename:variable
        // filename can be quoted with " as it can contain colons
        QRegExp r( "NETCDF:(.+):([^:]+)" );
        if ( r.exactMatch( mDataSource ) )
        {
          QString filename = r.cap( 1 );
          if ( filename.startsWith( '"' ) && filename.endsWith( '"' ) )
            filename = filename.mid( 1, filename.length() - 2 );
          mDataSource = "NETCDF:\"" + QgsProject::instance()->readPath( filename, relativeBasePath ) + "\":" + r.cap( 2 );
          handled = true;
        }
      }
      else if ( mDataSource.startsWith( "HDF4_SDS:" ) )
      {
        // HDF4_SDS:subdataset_type:file_name:subdataset_index
        // filename can be quoted with " as it can contain colons
        QRegExp r( "HDF4_SDS:([^:]+):(.+):([^:]+)" );
        if ( r.exactMatch( mDataSource ) )
        {
          QString filename = r.cap( 2 );
          if ( filename.startsWith( '"' ) && filename.endsWith( '"' ) )
            filename = filename.mid( 1, filename.length() - 2 );
          mDataSource = "HDF4_SDS:" + r.cap( 1 ) + ":\"" + QgsProject::instance()->readPath( filename, relativeBasePath ) + "\":" + r.cap( 3 );
          handled = true;
        }
      }
      else if ( mDataSource.startsWith( "HDF5:" ) )
      {
        // HDF5:file_name:subdataset
        // filename can be quoted with " as it can contain colons
        QRegExp r( "HDF5:(.+):([^:]+)" );
        if ( r.exactMatch( mDataSource ) )
        {
          QString filename = r.cap( 1 );
          if ( filename.startsWith( '"' ) && filename.endsWith( '"' ) )
            filename = filename.mid( 1, filename.length() - 2 );
          mDataSource = "HDF5:\"" + QgsProject::instance()->readPath( filename, relativeBasePath ) + "\":" + r.cap( 2 );
          handled = true;
        }
      }
      else if ( mDataSource.contains( QRegExp( "^(NITF_IM|RADARSAT_2_CALIB):" ) ) )
      {
        // NITF_IM:0:filename
        // RADARSAT_2_CALIB:?:filename
        QRegExp r( "([^:]+):([^:]+):(.+)" );
        if ( r.exactMatch( mDataSource ) )
        {
          mDataSource = r.cap( 1 ) + ':' + r.cap( 2 ) + ':' + QgsProject::instance()->readPath( r.cap( 3 ), relativeBasePath );
          handled = true;
        }
      }
    }

    if ( !handled )
      mDataSource = QgsProject::instance()->readPath( mDataSource, relativeBasePath );
  }

  // Set the CRS from project file, asking the user if necessary.
  // Make it the saved CRS to have WMS layer projected correctly.
  // We will still overwrite whatever GDAL etc picks up anyway
  // further down this function.
  mnl = layerElement.namedItem( "layername" );
  mne = mnl.toElement();

  QgsCoordinateReferenceSystem savedCRS;
  CUSTOM_CRS_VALIDATION savedValidation;

  QDomNode srsNode = layerElement.namedItem( "srs" );
  mCRS.readXML( srsNode );
  mCRS.setValidationHint( tr( "Specify CRS for layer %1" ).arg( mne.text() ) );
  mCRS.validate();
  savedCRS = mCRS;

  // Do not validate any projections in children, they will be overwritten anyway.
  // No need to ask the user for a projections when it is overwritten, is there?
  savedValidation = QgsCoordinateReferenceSystem::customSrsValidation();
  QgsCoordinateReferenceSystem::setCustomSrsValidation( nullptr );

  // now let the children grab what they need from the Dom node.
  layerError = !readXml( layerElement );

  // overwrite CRS with what we read from project file before the raster/vector
  // file readnig functions changed it. They will if projections is specfied in the file.
  // FIXME: is this necessary?
  QgsCoordinateReferenceSystem::setCustomSrsValidation( savedValidation );
  mCRS = savedCRS;

  // Abort if any error in layer, such as not found.
  if ( layerError )
  {
    return false;
  }

  // the internal name is just the data source basename
  //QFileInfo dataSourceFileInfo( mDataSource );
  //internalName = dataSourceFileInfo.baseName();

  // set ID
  mnl = layerElement.namedItem( "id" );
  if ( ! mnl.isNull() )
  {
    mne = mnl.toElement();
    if ( ! mne.isNull() && mne.text().length() > 10 ) // should be at least 17 (yyyyMMddhhmmsszzz)
    {
      mID = mne.text();
    }
  }

  // use scale dependent visibility flag
  setScaleBasedVisibility( layerElement.attribute( "hasScaleBasedVisibilityFlag" ).toInt() == 1 );
  setMinimumScale( layerElement.attribute( "minimumScale" ).toDouble() );
  setMaximumScale( layerElement.attribute( "maximumScale" ).toDouble() );

  QDomNode extentNode = layerElement.namedItem( "extent" );
  if ( !extentNode.isNull() )
  {
    setExtent( QgsXmlUtils::readRectangle( extentNode.toElement() ) );
  }

  // set name
  mnl = layerElement.namedItem( "layername" );
  mne = mnl.toElement();
  setName( mne.text() );

  //short name
  QDomElement shortNameElem = layerElement.firstChildElement( "shortname" );
  if ( !shortNameElem.isNull() )
  {
    mShortName = shortNameElem.text();
  }

  //title
  QDomElement titleElem = layerElement.firstChildElement( "title" );
  if ( !titleElem.isNull() )
  {
    mTitle = titleElem.text();
  }

  //abstract
  QDomElement abstractElem = layerElement.firstChildElement( "abstract" );
  if ( !abstractElem.isNull() )
  {
    mAbstract = abstractElem.text();
  }

  //keywordList
  QDomElement keywordListElem = layerElement.firstChildElement( "keywordList" );
  if ( !keywordListElem.isNull() )
  {
    QStringList kwdList;
    for ( QDomNode n = keywordListElem.firstChild(); !n.isNull(); n = n.nextSibling() )
    {
      kwdList << n.toElement().text();
    }
    mKeywordList = kwdList.join( ", " );
  }

  //metadataUrl
  QDomElement dataUrlElem = layerElement.firstChildElement( "dataUrl" );
  if ( !dataUrlElem.isNull() )
  {
    mDataUrl = dataUrlElem.text();
    mDataUrlFormat = dataUrlElem.attribute( "format", "" );
  }

  //legendUrl
  QDomElement legendUrlElem = layerElement.firstChildElement( "legendUrl" );
  if ( !legendUrlElem.isNull() )
  {
    mLegendUrl = legendUrlElem.text();
    mLegendUrlFormat = legendUrlElem.attribute( "format", "" );
  }

  //attribution
  QDomElement attribElem = layerElement.firstChildElement( "attribution" );
  if ( !attribElem.isNull() )
  {
    mAttribution = attribElem.text();
    mAttributionUrl = attribElem.attribute( "href", "" );
  }

  //metadataUrl
  QDomElement metaUrlElem = layerElement.firstChildElement( "metadataUrl" );
  if ( !metaUrlElem.isNull() )
  {
    mMetadataUrl = metaUrlElem.text();
    mMetadataUrlType = metaUrlElem.attribute( "type", "" );
    mMetadataUrlFormat = metaUrlElem.attribute( "format", "" );
  }

#if 0
  //read transparency level
  QDomNode transparencyNode = layer_node.namedItem( "transparencyLevelInt" );
  if ( ! transparencyNode.isNull() )
  {
    // set transparency level only if it's in project
    // (otherwise it sets the layer transparent)
    QDomElement myElement = transparencyNode.toElement();
    setTransparency( myElement.text().toInt() );
  }
#endif

  readCustomProperties( layerElement );

  return true;
} // bool QgsMapLayer::readLayerXML


bool QgsMapLayer::readXml( const QDomNode& layer_node )
{
  Q_UNUSED( layer_node );
  // NOP by default; children will over-ride with behavior specific to them

  return true;
} // void QgsMapLayer::readXml



bool QgsMapLayer::writeLayerXML( QDomElement& layerElement, QDomDocument& document, const QString& relativeBasePath )
{
  // use scale dependent visibility flag
  layerElement.setAttribute( "hasScaleBasedVisibilityFlag", hasScaleBasedVisibility() ? 1 : 0 );
  layerElement.setAttribute( "minimumScale", QString::number( minimumScale() ) );
  layerElement.setAttribute( "maximumScale", QString::number( maximumScale() ) );

  if ( !mExtent.isNull() )
  {
    layerElement.appendChild( QgsXmlUtils::writeRectangle( mExtent, document ) );
  }

  // ID
  QDomElement layerId = document.createElement( "id" );
  QDomText layerIdText = document.createTextNode( id() );
  layerId.appendChild( layerIdText );

  layerElement.appendChild( layerId );

  // data source
  QDomElement dataSource = document.createElement( "datasource" );

  QString src = source();

  QgsVectorLayer *vlayer = qobject_cast<QgsVectorLayer *>( this );
  // TODO: what about postgres, mysql and others, they should not go through writePath()
  if ( vlayer && vlayer->providerType() == "spatialite" )
  {
    QgsDataSourceURI uri( src );
    QString database = QgsProject::instance()->writePath( uri.database(), relativeBasePath );
    uri.setConnection( uri.host(), uri.port(), database, uri.username(), uri.password() );
    src = uri.uri();
  }
  else if ( vlayer && vlayer->providerType() == "ogr" )
  {
    QStringList theURIParts = src.split( '|' );
    theURIParts[0] = QgsProject::instance()->writePath( theURIParts[0], relativeBasePath );
    src = theURIParts.join( "|" );
  }
  else if ( vlayer && vlayer->providerType() == "gpx" )
  {
    QStringList theURIParts = src.split( '?' );
    theURIParts[0] = QgsProject::instance()->writePath( theURIParts[0], relativeBasePath );
    src = theURIParts.join( "?" );
  }
  else if ( vlayer && vlayer->providerType() == "delimitedtext" )
  {
    QUrl urlSource = QUrl::fromEncoded( src.toAscii() );
    QUrl urlDest = QUrl::fromLocalFile( QgsProject::instance()->writePath( urlSource.toLocalFile(), relativeBasePath ) );
    urlDest.setQueryItems( urlSource.queryItems() );
    src = QString::fromAscii( urlDest.toEncoded() );
  }
  else if ( vlayer && vlayer->providerType() == "memory" )
  {
    // Refetch the source from the provider, because adding fields actually changes the source for this provider.
    src = vlayer->dataProvider()->dataSourceUri();
  }
  else
  {
    bool handled = false;

    if ( !vlayer )
    {
      QgsRasterLayer *rlayer = qobject_cast<QgsRasterLayer *>( this );
      // Update path for subdataset
      if ( rlayer && rlayer->providerType() == "gdal" )
      {
        if ( src.startsWith( "NETCDF:" ) )
        {
          // NETCDF:filename:variable
          // filename can be quoted with " as it can contain colons
          QRegExp r( "NETCDF:(.+):([^:]+)" );
          if ( r.exactMatch( src ) )
          {
            QString filename = r.cap( 1 );
            if ( filename.startsWith( '"' ) && filename.endsWith( '"' ) )
              filename = filename.mid( 1, filename.length() - 2 );
            src = "NETCDF:\"" + QgsProject::instance()->writePath( filename, relativeBasePath ) + "\":" + r.cap( 2 );
            handled = true;
          }
        }
        else if ( src.startsWith( "HDF4_SDS:" ) )
        {
          // HDF4_SDS:subdataset_type:file_name:subdataset_index
          // filename can be quoted with " as it can contain colons
          QRegExp r( "HDF4_SDS:([^:]+):(.+):([^:]+)" );
          if ( r.exactMatch( src ) )
          {
            QString filename = r.cap( 2 );
            if ( filename.startsWith( '"' ) && filename.endsWith( '"' ) )
              filename = filename.mid( 1, filename.length() - 2 );
            src = "HDF4_SDS:" + r.cap( 1 ) + ":\"" + QgsProject::instance()->writePath( filename, relativeBasePath ) + "\":" + r.cap( 3 );
            handled = true;
          }
        }
        else if ( src.startsWith( "HDF5:" ) )
        {
          // HDF5:file_name:subdataset
          // filename can be quoted with " as it can contain colons
          QRegExp r( "HDF5:(.+):([^:]+)" );
          if ( r.exactMatch( src ) )
          {
            QString filename = r.cap( 1 );
            if ( filename.startsWith( '"' ) && filename.endsWith( '"' ) )
              filename = filename.mid( 1, filename.length() - 2 );
            src = "HDF5:\"" + QgsProject::instance()->writePath( filename, relativeBasePath ) + "\":" + r.cap( 2 );
            handled = true;
          }
        }
        else if ( src.contains( QRegExp( "^(NITF_IM|RADARSAT_2_CALIB):" ) ) )
        {
          // NITF_IM:0:filename
          // RADARSAT_2_CALIB:?:filename
          QRegExp r( "([^:]+):([^:]+):(.+)" );
          if ( r.exactMatch( src ) )
          {
            src = r.cap( 1 ) + ':' + r.cap( 2 ) + ':' + QgsProject::instance()->writePath( r.cap( 3 ), relativeBasePath );
            handled = true;
          }
        }
      }
    }

    if ( !handled )
      src = QgsProject::instance()->writePath( src, relativeBasePath );
  }

  QDomText dataSourceText = document.createTextNode( src );
  dataSource.appendChild( dataSourceText );

  layerElement.appendChild( dataSource );


  // layer name
  QDomElement layerName = document.createElement( "layername" );
  QDomText layerNameText = document.createTextNode( originalName() );
  layerName.appendChild( layerNameText );
  layerElement.appendChild( layerName );

  // layer short name
  if ( !mShortName.isEmpty() )
  {
    QDomElement layerShortName = document.createElement( "shortname" );
    QDomText layerShortNameText = document.createTextNode( mShortName );
    layerShortName.appendChild( layerShortNameText );
    layerElement.appendChild( layerShortName );
  }

  // layer title
  if ( !mTitle.isEmpty() )
  {
    QDomElement layerTitle = document.createElement( "title" );
    QDomText layerTitleText = document.createTextNode( mTitle );
    layerTitle.appendChild( layerTitleText );
    layerElement.appendChild( layerTitle );
  }

  // layer abstract
  if ( !mAbstract.isEmpty() )
  {
    QDomElement layerAbstract = document.createElement( "abstract" );
    QDomText layerAbstractText = document.createTextNode( mAbstract );
    layerAbstract.appendChild( layerAbstractText );
    layerElement.appendChild( layerAbstract );
  }

  // layer keyword list
  QStringList keywordStringList = keywordList().split( ',' );
  if ( !keywordStringList.isEmpty() )
  {
    QDomElement layerKeywordList = document.createElement( "keywordList" );
    for ( int i = 0; i < keywordStringList.size(); ++i )
    {
      QDomElement layerKeywordValue = document.createElement( "value" );
      QDomText layerKeywordText = document.createTextNode( keywordStringList.at( i ).trimmed() );
      layerKeywordValue.appendChild( layerKeywordText );
      layerKeywordList.appendChild( layerKeywordValue );
    }
    layerElement.appendChild( layerKeywordList );
  }

  // layer metadataUrl
  QString aDataUrl = dataUrl();
  if ( !aDataUrl.isEmpty() )
  {
    QDomElement layerDataUrl = document.createElement( "dataUrl" );
    QDomText layerDataUrlText = document.createTextNode( aDataUrl );
    layerDataUrl.appendChild( layerDataUrlText );
    layerDataUrl.setAttribute( "format", dataUrlFormat() );
    layerElement.appendChild( layerDataUrl );
  }

  // layer legendUrl
  QString aLegendUrl = legendUrl();
  if ( !aLegendUrl.isEmpty() )
  {
    QDomElement layerLegendUrl = document.createElement( "legendUrl" );
    QDomText layerLegendUrlText = document.createTextNode( aLegendUrl );
    layerLegendUrl.appendChild( layerLegendUrlText );
    layerLegendUrl.setAttribute( "format", legendUrlFormat() );
    layerElement.appendChild( layerLegendUrl );
  }

  // layer attribution
  QString aAttribution = attribution();
  if ( !aAttribution.isEmpty() )
  {
    QDomElement layerAttribution = document.createElement( "attribution" );
    QDomText layerAttributionText = document.createTextNode( aAttribution );
    layerAttribution.appendChild( layerAttributionText );
    layerAttribution.setAttribute( "href", attributionUrl() );
    layerElement.appendChild( layerAttribution );
  }

  // layer metadataUrl
  QString aMetadataUrl = metadataUrl();
  if ( !aMetadataUrl.isEmpty() )
  {
    QDomElement layerMetadataUrl = document.createElement( "metadataUrl" );
    QDomText layerMetadataUrlText = document.createTextNode( aMetadataUrl );
    layerMetadataUrl.appendChild( layerMetadataUrlText );
    layerMetadataUrl.setAttribute( "type", metadataUrlType() );
    layerMetadataUrl.setAttribute( "format", metadataUrlFormat() );
    layerElement.appendChild( layerMetadataUrl );
  }

  // timestamp if supported
  if ( timestamp() > QDateTime() )
  {
    QDomElement stamp = document.createElement( "timestamp" );
    QDomText stampText = document.createTextNode( timestamp().toString( Qt::ISODate ) );
    stamp.appendChild( stampText );
    layerElement.appendChild( stamp );
  }

  layerElement.appendChild( layerName );

  // zorder
  // This is no longer stored in the project file. It is superfluous since the layers
  // are written and read in the proper order.

  // spatial reference system id
  QDomElement mySrsElement = document.createElement( "srs" );
  mCRS.writeXML( mySrsElement, document );
  layerElement.appendChild( mySrsElement );

#if 0
  // <transparencyLevelInt>
  QDomElement transparencyLevelIntElement = document.createElement( "transparencyLevelInt" );
  QDomText    transparencyLevelIntText    = document.createTextNode( QString::number( getTransparency() ) );
  transparencyLevelIntElement.appendChild( transparencyLevelIntText );
  maplayer.appendChild( transparencyLevelIntElement );
#endif

  // now append layer node to map layer node

  writeCustomProperties( layerElement, document );

  return writeXml( layerElement, document );

} // bool QgsMapLayer::writeXML

QDomDocument QgsMapLayer::asLayerDefinition( const QList<QgsMapLayer *>& layers, const QString& relativeBasePath )
{
  QDomDocument doc( "qgis-layer-definition" );
  QDomElement qgiselm = doc.createElement( "qlr" );
  doc.appendChild( qgiselm );
  QDomElement layerselm = doc.createElement( "maplayers" );
  Q_FOREACH ( QgsMapLayer* layer, layers )
  {
    QDomElement layerelm = doc.createElement( "maplayer" );
    layer->writeLayerXML( layerelm, doc, relativeBasePath );
    layerselm.appendChild( layerelm );
  }
  qgiselm.appendChild( layerselm );
  return doc;
}

QList<QgsMapLayer*> QgsMapLayer::fromLayerDefinition( QDomDocument& document, bool addToRegistry, bool addToLegend, const QString& relativeBasePath )
{
  QList<QgsMapLayer*> layers;
  QDomNodeList layernodes = document.elementsByTagName( "maplayer" );
  for ( int i = 0; i < layernodes.size(); ++i )
  {
    QDomNode layernode = layernodes.at( i );
    QDomElement layerElem = layernode.toElement();

    QString type = layerElem.attribute( "type" );
    QgsDebugMsg( type );
    QgsMapLayer *layer = nullptr;

    if ( type == "vector" )
    {
      layer = new QgsVectorLayer;
    }
    else if ( type == "raster" )
    {
      layer = new QgsRasterLayer;
    }
    else if ( type == "plugin" )
    {
      QString typeName = layerElem.attribute( "name" );
      layer = QgsPluginLayerRegistry::instance()->createLayer( typeName );
    }

    if ( !layer )
      continue;

    bool ok = layer->readLayerXML( layerElem, relativeBasePath );
    if ( ok )
    {
      layers << layer;
      if ( addToRegistry )
        QgsMapLayerRegistry::instance()->addMapLayer( layer, addToLegend );
    }
  }
  return layers;
}

QList<QgsMapLayer *> QgsMapLayer::fromLayerDefinitionFile( const QString &qlrfile )
{
  QFile file( qlrfile );
  if ( !file.open( QIODevice::ReadOnly ) )
  {
    QgsDebugMsg( "Can't open file" );
    return QList<QgsMapLayer*>();
  }

  QDomDocument doc;
  if ( !doc.setContent( &file ) )
  {
    QgsDebugMsg( "Can't set content" );
    return QList<QgsMapLayer*>();
  }

  QFileInfo fileinfo( file );
  QDir::setCurrent( fileinfo.absoluteDir().path() );
  return QgsMapLayer::fromLayerDefinition( doc );
}


bool QgsMapLayer::writeXml( QDomNode & layer_node, QDomDocument & document )
{
  Q_UNUSED( layer_node );
  Q_UNUSED( document );
  // NOP by default; children will over-ride with behavior specific to them

  return true;
} // void QgsMapLayer::writeXml


void QgsMapLayer::readCustomProperties( const QDomNode &layerNode, const QString &keyStartsWith )
{
  mCustomProperties.readXml( layerNode, keyStartsWith );
}

void QgsMapLayer::writeCustomProperties( QDomNode &layerNode, QDomDocument &doc ) const
{
  mCustomProperties.writeXml( layerNode, doc );
}

void QgsMapLayer::readStyleManager( const QDomNode& layerNode )
{
  QDomElement styleMgrElem = layerNode.firstChildElement( "map-layer-style-manager" );
  if ( !styleMgrElem.isNull() )
    mStyleManager->readXml( styleMgrElem );
  else
    mStyleManager->reset();
}

void QgsMapLayer::writeStyleManager( QDomNode& layerNode, QDomDocument& doc ) const
{
  if ( mStyleManager )
  {
    QDomElement styleMgrElem = doc.createElement( "map-layer-style-manager" );
    mStyleManager->writeXml( styleMgrElem );
    layerNode.appendChild( styleMgrElem );
  }
}




bool QgsMapLayer::isValid()
{
  return mValid;
}


void QgsMapLayer::invalidTransformInput()
{
  QgsDebugMsg( "called" );
  // TODO: emit a signal - it will be used to update legend
}


QString QgsMapLayer::lastErrorTitle()
{
  return QString();
}

QString QgsMapLayer::lastError()
{
  return QString();
}

#if 0
void QgsMapLayer::connectNotify( const char * signal )
{
  Q_UNUSED( signal );
  QgsDebugMsgLevel( "QgsMapLayer connected to " + QString( signal ), 3 );
} //  QgsMapLayer::connectNotify
#endif

bool QgsMapLayer::isInScaleRange( double scale ) const
{
  return !mScaleBasedVisibility || ( mMinScale * QGis::SCALE_PRECISION < scale && scale < mMaxScale );
}

void QgsMapLayer::toggleScaleBasedVisibility( bool theVisibilityFlag )
{
  setScaleBasedVisibility( theVisibilityFlag );
}

bool QgsMapLayer::hasScaleBasedVisibility() const
{
  return mScaleBasedVisibility;
}

void QgsMapLayer::setMinimumScale( double theMinScale )
{
  mMinScale = theMinScale;
}

double QgsMapLayer::minimumScale() const
{
  return mMinScale;
}


void QgsMapLayer::setMaximumScale( double theMaxScale )
{
  mMaxScale = theMaxScale;
}

void QgsMapLayer::setScaleBasedVisibility( const bool enabled )
{
  mScaleBasedVisibility = enabled;
}

double QgsMapLayer::maximumScale() const
{
  return mMaxScale;
}

QStringList QgsMapLayer::subLayers() const
{
  return QStringList();  // Empty
}

void QgsMapLayer::setLayerOrder( const QStringList &layers )
{
  Q_UNUSED( layers );
  // NOOP
}

void QgsMapLayer::setSubLayerVisibility( const QString& name, bool vis )
{
  Q_UNUSED( name );
  Q_UNUSED( vis );
  // NOOP
}

const QgsCoordinateReferenceSystem& QgsMapLayer::crs() const
{
  return mCRS;
}

void QgsMapLayer::setCrs( const QgsCoordinateReferenceSystem& srs, bool emitSignal )
{
  mCRS = srs;

  if ( !mCRS.isValid() )
  {
    mCRS.setValidationHint( tr( "Specify CRS for layer %1" ).arg( name() ) );
    mCRS.validate();
  }

  if ( emitSignal )
    emit layerCrsChanged();
}

QString QgsMapLayer::capitaliseLayerName( const QString& name )
{
  // Capitalise the first letter of the layer name if requested
  QSettings settings;
  bool capitaliseLayerName =
    settings.value( "/qgis/capitaliseLayerName", QVariant( false ) ).toBool();

  QString layerName( name );

  if ( capitaliseLayerName && !layerName.isEmpty() )
    layerName = layerName.at( 0 ).toUpper() + layerName.mid( 1 );

  return layerName;
}

QString QgsMapLayer::styleURI()
{
  QString myURI = publicSource();

  // if file is using the VSIFILE mechanism, remove the prefix
  if ( myURI.startsWith( "/vsigzip/", Qt::CaseInsensitive ) )
  {
    myURI.remove( 0, 9 );
  }
  else if ( myURI.startsWith( "/vsizip/", Qt::CaseInsensitive ) &&
            myURI.endsWith( ".zip", Qt::CaseInsensitive ) )
  {
    // ideally we should look for .qml file inside zip file
    myURI.remove( 0, 8 );
  }
  else if ( myURI.startsWith( "/vsitar/", Qt::CaseInsensitive ) &&
            ( myURI.endsWith( ".tar", Qt::CaseInsensitive ) ||
              myURI.endsWith( ".tar.gz", Qt::CaseInsensitive ) ||
              myURI.endsWith( ".tgz", Qt::CaseInsensitive ) ) )
  {
    // ideally we should look for .qml file inside tar file
    myURI.remove( 0, 8 );
  }

  QFileInfo myFileInfo( myURI );
  QString key;

  if ( myFileInfo.exists() )
  {
    // if file is using the /vsizip/ or /vsigzip/ mechanism, cleanup the name
    if ( myURI.endsWith( ".gz", Qt::CaseInsensitive ) )
      myURI.chop( 3 );
    else if ( myURI.endsWith( ".zip", Qt::CaseInsensitive ) )
      myURI.chop( 4 );
    else if ( myURI.endsWith( ".tar", Qt::CaseInsensitive ) )
      myURI.chop( 4 );
    else if ( myURI.endsWith( ".tar.gz", Qt::CaseInsensitive ) )
      myURI.chop( 7 );
    else if ( myURI.endsWith( ".tgz", Qt::CaseInsensitive ) )
      myURI.chop( 4 );
    myFileInfo.setFile( myURI );
    // get the file name for our .qml style file
    key = myFileInfo.path() + QDir::separator() + myFileInfo.completeBaseName() + ".qml";
  }
  else
  {
    key = publicSource();
  }

  return key;
}

QString QgsMapLayer::loadDefaultStyle( bool & theResultFlag )
{
  return loadNamedStyle( styleURI(), theResultFlag );
}

bool QgsMapLayer::loadNamedStyleFromDb( const QString &db, const QString &theURI, QString &qml )
{
  QgsDebugMsg( QString( "db = %1 uri = %2" ).arg( db, theURI ) );

  bool theResultFlag = false;

  // read from database
  sqlite3 *myDatabase;
  sqlite3_stmt *myPreparedStatement;
  const char *myTail;
  int myResult;

  QgsDebugMsg( QString( "Trying to load style for \"%1\" from \"%2\"" ).arg( theURI, db ) );

  if ( db.isEmpty() || !QFile( db ).exists() )
    return false;

  myResult = sqlite3_open_v2( db.toUtf8().data(), &myDatabase, SQLITE_OPEN_READONLY, nullptr );
  if ( myResult != SQLITE_OK )
  {
    return false;
  }

  QString mySql = "select qml from tbl_styles where style=?";
  myResult = sqlite3_prepare( myDatabase, mySql.toUtf8().data(), mySql.toUtf8().length(), &myPreparedStatement, &myTail );
  if ( myResult == SQLITE_OK )
  {
    QByteArray param = theURI.toUtf8();

    if ( sqlite3_bind_text( myPreparedStatement, 1, param.data(), param.length(), SQLITE_STATIC ) == SQLITE_OK &&
         sqlite3_step( myPreparedStatement ) == SQLITE_ROW )
    {
      qml = QString::fromUtf8( reinterpret_cast< const char * >( sqlite3_column_text( myPreparedStatement, 0 ) ) );
      theResultFlag = true;
    }

    sqlite3_finalize( myPreparedStatement );
  }

  sqlite3_close( myDatabase );

  return theResultFlag;
}


QString QgsMapLayer::loadNamedStyle( const QString &theURI, bool &theResultFlag )
{
  QgsDebugMsg( QString( "uri = %1 myURI = %2" ).arg( theURI, publicSource() ) );

  theResultFlag = false;

  QDomDocument myDocument( "qgis" );

  // location of problem associated with errorMsg
  int line, column;
  QString myErrorMessage;

  QFile myFile( theURI );
  if ( myFile.open( QFile::ReadOnly ) )
  {
    // read file
    theResultFlag = myDocument.setContent( &myFile, &myErrorMessage, &line, &column );
    if ( !theResultFlag )
      myErrorMessage = tr( "%1 at line %2 column %3" ).arg( myErrorMessage ).arg( line ).arg( column );
    myFile.close();
  }
  else
  {
    QFileInfo project( QgsProject::instance()->fileName() );
    QgsDebugMsg( QString( "project fileName: %1" ).arg( project.absoluteFilePath() ) );

    QString qml;
    if ( loadNamedStyleFromDb( QDir( QgsApplication::qgisSettingsDirPath() ).absoluteFilePath( "qgis.qmldb" ), theURI, qml ) ||
         ( project.exists() && loadNamedStyleFromDb( project.absoluteDir().absoluteFilePath( project.baseName() + ".qmldb" ), theURI, qml ) ) ||
         loadNamedStyleFromDb( QDir( QgsApplication::pkgDataPath() ).absoluteFilePath( "resources/qgis.qmldb" ), theURI, qml ) )
    {
      theResultFlag = myDocument.setContent( qml, &myErrorMessage, &line, &column );
      if ( !theResultFlag )
      {
        myErrorMessage = tr( "%1 at line %2 column %3" ).arg( myErrorMessage ).arg( line ).arg( column );
      }
    }
    else
    {
      myErrorMessage = tr( "Style not found in database" );
    }
  }

  if ( !theResultFlag )
  {
    return myErrorMessage;
  }

  theResultFlag = importNamedStyle( myDocument, myErrorMessage );
  if ( !theResultFlag )
    myErrorMessage = tr( "Loading style file %1 failed because:\n%2" ).arg( theURI, myErrorMessage );

  return myErrorMessage;
}


bool QgsMapLayer::importNamedStyle( QDomDocument& myDocument, QString& myErrorMessage )
{
  QDomElement myRoot = myDocument.firstChildElement( "qgis" );
  if ( myRoot.isNull() )
  {
    myErrorMessage = tr( "Root <qgis> element could not be found" );
    return false;
  }

  // get style file version string, if any
  QgsProjectVersion fileVersion( myRoot.attribute( "version" ) );
  QgsProjectVersion thisVersion( QGis::QGIS_VERSION );

  if ( thisVersion > fileVersion )
  {
    QgsProjectFileTransform styleFile( myDocument, fileVersion );
    styleFile.updateRevision( thisVersion );
  }

  //Test for matching geometry type on vector layers when applying, if geometry type is given in the style
  if ( type() == QgsMapLayer::VectorLayer && !myRoot.firstChildElement( "layerGeometryType" ).isNull() )
  {
    QgsVectorLayer *vl = static_cast<QgsVectorLayer*>( this );
    int importLayerGeometryType = myRoot.firstChildElement( "layerGeometryType" ).text().toInt();
    if ( vl->geometryType() != importLayerGeometryType )
    {
      myErrorMessage = tr( "Cannot apply style to layer with a different geometry type" );
      return false;
    }
  }

  // use scale dependent visibility flag
  setScaleBasedVisibility( myRoot.attribute( "hasScaleBasedVisibilityFlag" ).toInt() == 1 );
  setMinimumScale( myRoot.attribute( "minimumScale" ).toDouble() );
  setMaximumScale( myRoot.attribute( "maximumScale" ).toDouble() );

#if 0
  //read transparency level
  QDomNode transparencyNode = myRoot.namedItem( "transparencyLevelInt" );
  if ( ! transparencyNode.isNull() )
  {
    // set transparency level only if it's in project
    // (otherwise it sets the layer transparent)
    QDomElement myElement = transparencyNode.toElement();
    setTransparency( myElement.text().toInt() );
  }
#endif

  return readSymbology( myRoot, myErrorMessage );
}

void QgsMapLayer::exportNamedStyle( QDomDocument &doc, QString &errorMsg )
{
  QDomImplementation DomImplementation;
  QDomDocumentType documentType = DomImplementation.createDocumentType( "qgis", "http://mrcc.com/qgis.dtd", "SYSTEM" );
  QDomDocument myDocument( documentType );

  QDomElement myRootNode = myDocument.createElement( "qgis" );
  myRootNode.setAttribute( "version", QGis::QGIS_VERSION );
  myDocument.appendChild( myRootNode );

  myRootNode.setAttribute( "hasScaleBasedVisibilityFlag", hasScaleBasedVisibility() ? 1 : 0 );
  myRootNode.setAttribute( "minimumScale", QString::number( minimumScale() ) );
  myRootNode.setAttribute( "maximumScale", QString::number( maximumScale() ) );

#if 0
  // <transparencyLevelInt>
  QDomElement transparencyLevelIntElement = myDocument.createElement( "transparencyLevelInt" );
  QDomText    transparencyLevelIntText    = myDocument.createTextNode( QString::number( getTransparency() ) );
  transparencyLevelIntElement.appendChild( transparencyLevelIntText );
  myRootNode.appendChild( transparencyLevelIntElement );
#endif

  if ( !writeSymbology( myRootNode, myDocument, errorMsg ) )
  {
    errorMsg = QObject::tr( "Could not save symbology because:\n%1" ).arg( errorMsg );
    return;
  }

  /*
   * Check to see if the layer is vector - in which case we should also export its geometryType
   * to avoid eventually pasting to a layer with a different geometry
  */
  if ( type() == QgsMapLayer::VectorLayer )
  {
    //Getting the selectionLayer geometry
    QgsVectorLayer *vl = static_cast<QgsVectorLayer*>( this );
    QString geoType = QString::number( vl->geometryType() );

    //Adding geometryinformation
    QDomElement layerGeometryType = myDocument.createElement( "layerGeometryType" );
    QDomText type = myDocument.createTextNode( geoType );

    layerGeometryType.appendChild( type );
    myRootNode.appendChild( layerGeometryType );
  }

  doc = myDocument;
}

QString QgsMapLayer::saveDefaultStyle( bool & theResultFlag )
{
  return saveNamedStyle( styleURI(), theResultFlag );
}

QString QgsMapLayer::saveNamedStyle( const QString &theURI, bool &theResultFlag )
{
  QString myErrorMessage;
  QDomDocument myDocument;
  exportNamedStyle( myDocument, myErrorMessage );

  // check if the uri is a file or ends with .qml,
  // which indicates that it should become one
  // everything else goes to the database
  QString filename;

  QgsVectorLayer *vlayer = qobject_cast<QgsVectorLayer *>( this );
  if ( vlayer && vlayer->providerType() == "ogr" )
  {
    QStringList theURIParts = theURI.split( '|' );
    filename = theURIParts[0];
  }
  else if ( vlayer && vlayer->providerType() == "gpx" )
  {
    QStringList theURIParts = theURI.split( '?' );
    filename = theURIParts[0];
  }
  else if ( vlayer && vlayer->providerType() == "delimitedtext" )
  {
    filename = QUrl::fromEncoded( theURI.toAscii() ).toLocalFile();
    // toLocalFile() returns an empty string if theURI is a plain Windows-path, e.g. "C:/style.qml"
    if ( filename.isEmpty() )
      filename = theURI;
  }
  else
  {
    filename = theURI;
  }

  QFileInfo myFileInfo( filename );
  if ( myFileInfo.exists() || filename.endsWith( ".qml", Qt::CaseInsensitive ) )
  {
    QFileInfo myDirInfo( myFileInfo.path() );  //excludes file name
    if ( !myDirInfo.isWritable() )
    {
      return tr( "The directory containing your dataset needs to be writable!" );
    }

    // now construct the file name for our .qml style file
    QString myFileName = myFileInfo.path() + QDir::separator() + myFileInfo.completeBaseName() + ".qml";

    QFile myFile( myFileName );
    if ( myFile.open( QFile::WriteOnly | QFile::Truncate ) )
    {
      QTextStream myFileStream( &myFile );
      // save as utf-8 with 2 spaces for indents
      myDocument.save( myFileStream, 2 );
      myFile.close();
      theResultFlag = true;
      return tr( "Created default style file as %1" ).arg( myFileName );
    }
    else
    {
      theResultFlag = false;
      return tr( "ERROR: Failed to created default style file as %1. Check file permissions and retry." ).arg( myFileName );
    }
  }
  else
  {
    QString qml = myDocument.toString();

    // read from database
    sqlite3 *myDatabase;
    sqlite3_stmt *myPreparedStatement;
    const char *myTail;
    int myResult;

    myResult = sqlite3_open( QDir( QgsApplication::qgisSettingsDirPath() ).absoluteFilePath( "qgis.qmldb" ).toUtf8().data(), &myDatabase );
    if ( myResult != SQLITE_OK )
    {
      return tr( "User database could not be opened." );
    }

    QByteArray param0 = theURI.toUtf8();
    QByteArray param1 = qml.toUtf8();

    QString mySql = "create table if not exists tbl_styles(style varchar primary key,qml varchar)";
    myResult = sqlite3_prepare( myDatabase, mySql.toUtf8().data(), mySql.toUtf8().length(), &myPreparedStatement, &myTail );
    if ( myResult == SQLITE_OK )
    {
      if ( sqlite3_step( myPreparedStatement ) != SQLITE_DONE )
      {
        sqlite3_finalize( myPreparedStatement );
        sqlite3_close( myDatabase );
        theResultFlag = false;
        return tr( "The style table could not be created." );
      }
    }

    sqlite3_finalize( myPreparedStatement );

    mySql = "insert into tbl_styles(style,qml) values (?,?)";
    myResult = sqlite3_prepare( myDatabase, mySql.toUtf8().data(), mySql.toUtf8().length(), &myPreparedStatement, &myTail );
    if ( myResult == SQLITE_OK )
    {
      if ( sqlite3_bind_text( myPreparedStatement, 1, param0.data(), param0.length(), SQLITE_STATIC ) == SQLITE_OK &&
           sqlite3_bind_text( myPreparedStatement, 2, param1.data(), param1.length(), SQLITE_STATIC ) == SQLITE_OK &&
           sqlite3_step( myPreparedStatement ) == SQLITE_DONE )
      {
        theResultFlag = true;
        myErrorMessage = tr( "The style %1 was saved to database" ).arg( theURI );
      }
    }

    sqlite3_finalize( myPreparedStatement );

    if ( !theResultFlag )
    {
      QString mySql = "update tbl_styles set qml=? where style=?";
      myResult = sqlite3_prepare( myDatabase, mySql.toUtf8().data(), mySql.toUtf8().length(), &myPreparedStatement, &myTail );
      if ( myResult == SQLITE_OK )
      {
        if ( sqlite3_bind_text( myPreparedStatement, 2, param0.data(), param0.length(), SQLITE_STATIC ) == SQLITE_OK &&
             sqlite3_bind_text( myPreparedStatement, 1, param1.data(), param1.length(), SQLITE_STATIC ) == SQLITE_OK &&
             sqlite3_step( myPreparedStatement ) == SQLITE_DONE )
        {
          theResultFlag = true;
          myErrorMessage = tr( "The style %1 was updated in the database." ).arg( theURI );
        }
        else
        {
          theResultFlag = false;
          myErrorMessage = tr( "The style %1 could not be updated in the database." ).arg( theURI );
        }
      }
      else
      {
        theResultFlag = false;
        myErrorMessage = tr( "The style %1 could not be inserted into database." ).arg( theURI );
      }

      sqlite3_finalize( myPreparedStatement );
    }

    sqlite3_close( myDatabase );
  }

  return myErrorMessage;
}

void QgsMapLayer::exportSldStyle( QDomDocument &doc, QString &errorMsg )
{
  QDomDocument myDocument = QDomDocument();

  QDomNode header = myDocument.createProcessingInstruction( "xml", "version=\"1.0\" encoding=\"UTF-8\"" );
  myDocument.appendChild( header );

  // Create the root element
  QDomElement root = myDocument.createElementNS( "http://www.opengis.net/sld", "StyledLayerDescriptor" );
  root.setAttribute( "version", "1.1.0" );
  root.setAttribute( "xsi:schemaLocation", "http://www.opengis.net/sld http://schemas.opengis.net/sld/1.1.0/StyledLayerDescriptor.xsd" );
  root.setAttribute( "xmlns:ogc", "http://www.opengis.net/ogc" );
  root.setAttribute( "xmlns:se", "http://www.opengis.net/se" );
  root.setAttribute( "xmlns:xlink", "http://www.w3.org/1999/xlink" );
  root.setAttribute( "xmlns:xsi", "http://www.w3.org/2001/XMLSchema-instance" );
  myDocument.appendChild( root );

  // Create the NamedLayer element
  QDomElement namedLayerNode = myDocument.createElement( "NamedLayer" );
  root.appendChild( namedLayerNode );

  QgsVectorLayer *vlayer = qobject_cast<QgsVectorLayer *>( this );
  if ( !vlayer )
  {
    errorMsg = tr( "Could not save symbology because:\n%1" )
               .arg( "Non-vector layers not supported yet" );
    return;
  }

  QgsStringMap props;
  if ( hasScaleBasedVisibility() )
  {
    props[ "scaleMinDenom" ] = QString::number( mMinScale );
    props[ "scaleMaxDenom" ] = QString::number( mMaxScale );
  }
  if ( !vlayer->writeSld( namedLayerNode, myDocument, errorMsg, props ) )
  {
    errorMsg = tr( "Could not save symbology because:\n%1" ).arg( errorMsg );
    return;
  }

  doc = myDocument;
}

QString QgsMapLayer::saveSldStyle( const QString &theURI, bool &theResultFlag )
{
  QString errorMsg;
  QDomDocument myDocument;
  exportSldStyle( myDocument, errorMsg );
  if ( !errorMsg.isNull() )
  {
    theResultFlag = false;
    return errorMsg;
  }
  QgsVectorLayer *vlayer = qobject_cast<QgsVectorLayer *>( this );

  // check if the uri is a file or ends with .sld,
  // which indicates that it should become one
  QString filename;
  if ( vlayer->providerType() == "ogr" )
  {
    QStringList theURIParts = theURI.split( '|' );
    filename = theURIParts[0];
  }
  else if ( vlayer->providerType() == "gpx" )
  {
    QStringList theURIParts = theURI.split( '?' );
    filename = theURIParts[0];
  }
  else if ( vlayer->providerType() == "delimitedtext" )
  {
    filename = QUrl::fromEncoded( theURI.toAscii() ).toLocalFile();
    // toLocalFile() returns an empty string if theURI is a plain Windows-path, e.g. "C:/style.qml"
    if ( filename.isEmpty() )
      filename = theURI;
  }
  else
  {
    filename = theURI;
  }

  QFileInfo myFileInfo( filename );
  if ( myFileInfo.exists() || filename.endsWith( ".sld", Qt::CaseInsensitive ) )
  {
    QFileInfo myDirInfo( myFileInfo.path() );  //excludes file name
    if ( !myDirInfo.isWritable() )
    {
      return tr( "The directory containing your dataset needs to be writable!" );
    }

    // now construct the file name for our .sld style file
    QString myFileName = myFileInfo.path() + QDir::separator() + myFileInfo.completeBaseName() + ".sld";

    QFile myFile( myFileName );
    if ( myFile.open( QFile::WriteOnly | QFile::Truncate ) )
    {
      QTextStream myFileStream( &myFile );
      // save as utf-8 with 2 spaces for indents
      myDocument.save( myFileStream, 2 );
      myFile.close();
      theResultFlag = true;
      return tr( "Created default style file as %1" ).arg( myFileName );
    }
  }

  theResultFlag = false;
  return tr( "ERROR: Failed to created SLD style file as %1. Check file permissions and retry." ).arg( filename );
}

QString QgsMapLayer::loadSldStyle( const QString &theURI, bool &theResultFlag )
{
  QgsDebugMsg( "Entered." );

  theResultFlag = false;

  QDomDocument myDocument;

  // location of problem associated with errorMsg
  int line, column;
  QString myErrorMessage;

  QFile myFile( theURI );
  if ( myFile.open( QFile::ReadOnly ) )
  {
    // read file
    theResultFlag = myDocument.setContent( &myFile, true, &myErrorMessage, &line, &column );
    if ( !theResultFlag )
      myErrorMessage = tr( "%1 at line %2 column %3" ).arg( myErrorMessage ).arg( line ).arg( column );
    myFile.close();
  }
  else
  {
    myErrorMessage = tr( "Unable to open file %1" ).arg( theURI );
  }

  if ( !theResultFlag )
  {
    return myErrorMessage;
  }

  // check for root SLD element
  QDomElement myRoot = myDocument.firstChildElement( "StyledLayerDescriptor" );
  if ( myRoot.isNull() )
  {
    myErrorMessage = QString( "Error: StyledLayerDescriptor element not found in %1" ).arg( theURI );
    theResultFlag = false;
    return myErrorMessage;
  }

  // now get the style node out and pass it over to the layer
  // to deserialise...
  QDomElement namedLayerElem = myRoot.firstChildElement( "NamedLayer" );
  if ( namedLayerElem.isNull() )
  {
    myErrorMessage = QLatin1String( "Info: NamedLayer element not found." );
    theResultFlag = false;
    return myErrorMessage;
  }

  QString errorMsg;
  theResultFlag = readSld( namedLayerElem, errorMsg );
  if ( !theResultFlag )
  {
    myErrorMessage = tr( "Loading style file %1 failed because:\n%2" ).arg( theURI, errorMsg );
    return myErrorMessage;
  }

  return "";
}

bool QgsMapLayer::readStyle( const QDomNode& node, QString& errorMessage )
{
  Q_UNUSED( node );
  Q_UNUSED( errorMessage );
  return false;
}

bool QgsMapLayer::writeStyle( QDomNode& node, QDomDocument& doc, QString& errorMessage ) const
{
  Q_UNUSED( node );
  Q_UNUSED( doc );
  Q_UNUSED( errorMessage );
  return false;
}


QUndoStack* QgsMapLayer::undoStack()
{
  return &mUndoStack;
}

QUndoStack* QgsMapLayer::undoStackStyles()
{
  return &mUndoStackStyles;
}


void QgsMapLayer::setCustomProperty( const QString& key, const QVariant& value )
{
  mCustomProperties.setValue( key, value );
}

QVariant QgsMapLayer::customProperty( const QString& value, const QVariant& defaultValue ) const
{
  return mCustomProperties.value( value, defaultValue );
}

void QgsMapLayer::removeCustomProperty( const QString& key )
{
  mCustomProperties.remove( key );
}



bool QgsMapLayer::isEditable() const
{
  return false;
}

void QgsMapLayer::setValid( bool valid )
{
  mValid = valid;
}

void QgsMapLayer::setCacheImage( QImage * )
{
  emit repaintRequested();
}

void QgsMapLayer::setLegend( QgsMapLayerLegend* legend )
{
  if ( legend == mLegend )
    return;

  delete mLegend;
  mLegend = legend;

  if ( mLegend )
    connect( mLegend, SIGNAL( itemsChanged() ), this, SIGNAL( legendChanged() ) );

  emit legendChanged();
}

QgsMapLayerLegend*QgsMapLayer::legend() const
{
  return mLegend;
}

QgsMapLayerStyleManager* QgsMapLayer::styleManager() const
{
  return mStyleManager;
}

void QgsMapLayer::clearCacheImage()
{
  emit repaintRequested();
}

void QgsMapLayer::triggerRepaint()
{
  emit repaintRequested();
}

QString QgsMapLayer::metadata()
{
  return QString();
}

void QgsMapLayer::emitStyleChanged()
{
  emit styleChanged();
}

void QgsMapLayer::setExtent( const QgsRectangle &r )
{
  mExtent = r;
}
