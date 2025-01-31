/***************************************************************************
    qgswmsdataitems.cpp
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
#include "qgswmsdataitems.h"

#include "qgslogger.h"

#include "qgsdataitemproviderregistry.h"
#include "qgsdatasourceuri.h"
#include "qgswmscapabilities.h"
#include "qgswmsconnection.h"
#include "qgswmssourceselect.h"
#include "qgsnewhttpconnection.h"
#include "qgstilescalewidget.h"
#include "qgscrscache.h"
#include "qgsxyzconnection.h"

#include <QInputDialog>

// ---------------------------------------------------------------------------
QgsWMSConnectionItem::QgsWMSConnectionItem( QgsDataItem* parent, QString name, QString path, QString uri )
    : QgsDataCollectionItem( parent, name, path )
    , mUri( uri )
    , mCapabilitiesDownload( nullptr )
{
  mIconName = "mIconConnect.png";
  mCapabilities |= Collapse;
  mCapabilitiesDownload = new QgsWmsCapabilitiesDownload( false );
}

QgsWMSConnectionItem::~QgsWMSConnectionItem()
{
  delete mCapabilitiesDownload;
}

void QgsWMSConnectionItem::deleteLater()
{
  if ( mCapabilitiesDownload )
  {
    mCapabilitiesDownload->abort();
  }
  QgsDataCollectionItem::deleteLater();
}

QVector<QgsDataItem*> QgsWMSConnectionItem::createChildren()
{
  QVector<QgsDataItem*> children;

  QgsDataSourceURI uri;
  uri.setEncodedUri( mUri );

  QgsDebugMsg( "mUri = " + mUri );

  QgsWmsSettings wmsSettings;
  if ( !wmsSettings.parseUri( mUri ) )
  {
    children.append( new QgsErrorItem( this, tr( "Failed to parse WMS URI" ), mPath + "/error" ) );
    return children;
  }

  bool res = mCapabilitiesDownload->downloadCapabilities( wmsSettings.baseUrl(), wmsSettings.authorization() );

  if ( !res )
  {
    children.append( new QgsErrorItem( this, tr( "Failed to download capabilities" ), mPath + "/error" ) );
    return children;
  }

  QgsWmsCapabilities caps;
  if ( !caps.parseResponse( mCapabilitiesDownload->response(), wmsSettings.parserSettings() ) )
  {
    children.append( new QgsErrorItem( this, tr( "Failed to parse capabilities" ), mPath + "/error" ) );
    return children;
  }

  // Attention: supportedLayers() gives tree leafs, not top level
  QVector<QgsWmsLayerProperty> layerProperties = caps.supportedLayers();
  if ( !layerProperties.isEmpty() )
  {
    QgsWmsCapabilitiesProperty capabilitiesProperty = caps.capabilitiesProperty();
    const QgsWmsCapabilityProperty& capabilityProperty = capabilitiesProperty.capability;

    // If we have several top-level layers, or if we just have one single top-level layer,
    // then use those top-level layers directly
    if ( capabilityProperty.layers.size() > 1 ||
         ( capabilityProperty.layers.size() == 1 && capabilityProperty.layers[0].layer.size() == 0 ) )
    {
      Q_FOREACH ( const QgsWmsLayerProperty& layerProperty, capabilityProperty.layers )
      {
        // Attention, the name may be empty
        QgsDebugMsg( QString::number( layerProperty.orderId ) + ' ' + layerProperty.name + ' ' + layerProperty.title );
        QString pathName = layerProperty.name.isEmpty() ? QString::number( layerProperty.orderId ) : layerProperty.name;

        QgsWMSLayerItem *layer = new QgsWMSLayerItem( this, layerProperty.title, mPath + '/' + pathName, capabilitiesProperty, uri, layerProperty );

        children << layer;
      }
    }
    // Otherwise if we have just one single top-level layers with children, then
    // skip this top-level layer and iterate directly on its children
    // Note (E. Rouault): this was the historical behaviour before fixing #13762
    else if ( capabilityProperty.layers.size() == 1 )
    {
      Q_FOREACH ( const QgsWmsLayerProperty &layerProperty, capabilityProperty.layers[0].layer )
      {
        // Attention, the name may be empty
        QgsDebugMsg( QString::number( layerProperty.orderId ) + ' ' + layerProperty.name + ' ' + layerProperty.title );
        QString pathName = layerProperty.name.isEmpty() ? QString::number( layerProperty.orderId ) : layerProperty.name;

        QgsWMSLayerItem *layer = new QgsWMSLayerItem( this, layerProperty.title, mPath + '/' + pathName, capabilitiesProperty, uri, layerProperty );

        children << layer;
      }
    }
  }

  QList<QgsWmtsTileLayer> tileLayers = caps.supportedTileLayers();
  if ( !tileLayers.isEmpty() )
  {
    QHash<QString, QgsWmtsTileMatrixSet> tileMatrixSets = caps.supportedTileMatrixSets();

    Q_FOREACH ( const QgsWmtsTileLayer &l, tileLayers )
    {
      QString title = l.title.isEmpty() ? l.identifier : l.title;
      QgsDataItem *layerItem = l.styles.size() == 1 ? this : new QgsDataCollectionItem( this, title, mPath + '/' + l.identifier );
      if ( layerItem != this )
      {
        layerItem->setCapabilities( layerItem->capabilities2() & ~QgsDataItem::Fertile );
        layerItem->setState( QgsDataItem::Populated );
        layerItem->setToolTip( title );
        children << layerItem;
      }

      Q_FOREACH ( const QgsWmtsStyle &style, l.styles )
      {
        QString styleName = style.title.isEmpty() ? style.identifier : style.title;
        if ( layerItem == this )
          styleName = title;  // just one style so no need to display it

        QgsDataItem *styleItem = l.setLinks.size() == 1 ? layerItem : new QgsDataCollectionItem( layerItem, styleName, layerItem->path() + '/' + style.identifier );
        if ( styleItem != layerItem )
        {
          styleItem->setCapabilities( styleItem->capabilities2() & ~QgsDataItem::Fertile );
          styleItem->setState( QgsDataItem::Populated );
          styleItem->setToolTip( styleName );
          if ( layerItem == this )
            children << styleItem;
          else
            layerItem->addChildItem( styleItem );
        }

        Q_FOREACH ( const QgsWmtsTileMatrixSetLink &setLink, l.setLinks )
        {
          QString linkName = setLink.tileMatrixSet;
          if ( styleItem == layerItem )
            linkName = styleName;  // just one link so no need to display it

          QgsDataItem *linkItem = l.formats.size() == 1 ? styleItem : new QgsDataCollectionItem( styleItem, linkName, styleItem->path() + '/' + setLink.tileMatrixSet );
          if ( linkItem != styleItem )
          {
            linkItem->setCapabilities( linkItem->capabilities2() & ~QgsDataItem::Fertile );
            linkItem->setState( QgsDataItem::Populated );
            linkItem->setToolTip( linkName );
            if ( styleItem == this )
              children << linkItem;
            else
              styleItem->addChildItem( linkItem );
          }

          Q_FOREACH ( const QString& format, l.formats )
          {
            QString name = format;
            if ( linkItem == styleItem )
              name = linkName;  // just one format so no need to display it

            QgsDataItem *tileLayerItem = new QgsWMTSLayerItem( linkItem, name, linkItem->path() + '/' + name, uri,
                l.identifier, format, style.identifier, setLink.tileMatrixSet, tileMatrixSets[ setLink.tileMatrixSet ].crs, title );
            tileLayerItem->setToolTip( name );
            if ( linkItem == this )
              children << tileLayerItem;
            else
              linkItem->addChildItem( tileLayerItem );
          }
        }
      }
    }
  }

  return children;
}

bool QgsWMSConnectionItem::equal( const QgsDataItem *other )
{
  if ( type() != other->type() )
  {
    return false;
  }
  const QgsWMSConnectionItem *o = dynamic_cast<const QgsWMSConnectionItem *>( other );
  if ( !o )
  {
    return false;
  }

  return ( mPath == o->mPath && mName == o->mName );
}

QList<QAction*> QgsWMSConnectionItem::actions()
{
  QList<QAction*> lst;

  QAction* actionEdit = new QAction( tr( "Edit..." ), this );
  connect( actionEdit, SIGNAL( triggered() ), this, SLOT( editConnection() ) );
  lst.append( actionEdit );

  QAction* actionDelete = new QAction( tr( "Delete" ), this );
  connect( actionDelete, SIGNAL( triggered() ), this, SLOT( deleteConnection() ) );
  lst.append( actionDelete );

  return lst;
}

void QgsWMSConnectionItem::editConnection()
{
  QgsNewHttpConnection nc( nullptr, "/Qgis/connections-wms/", mName );

  if ( nc.exec() )
  {
    // the parent should be updated
    mParent->refresh();
  }
}

void QgsWMSConnectionItem::deleteConnection()
{
  QgsWMSConnection::deleteConnection( mName );
  // the parent should be updated
  mParent->refresh();
}


// ---------------------------------------------------------------------------

QgsWMSLayerItem::QgsWMSLayerItem( QgsDataItem* parent, QString name, QString path, const QgsWmsCapabilitiesProperty &capabilitiesProperty, const QgsDataSourceURI& dataSourceUri, const QgsWmsLayerProperty &layerProperty )
    : QgsLayerItem( parent, name, path, QString(), QgsLayerItem::Raster, "wms" )
    , mCapabilitiesProperty( capabilitiesProperty )
    , mDataSourceUri( dataSourceUri )
    , mLayerProperty( layerProperty )
{
  mSupportedCRS = mLayerProperty.crs;
  mSupportFormats = mCapabilitiesProperty.capability.request.getMap.format;
  QgsDebugMsg( "uri = " + mDataSourceUri.encodedUri() );

  mUri = createUri();

  // Populate everything, it costs nothing, all info about layers is collected
  Q_FOREACH ( const QgsWmsLayerProperty &layerProperty, mLayerProperty.layer )
  {
    // Attention, the name may be empty
    QgsDebugMsg( QString::number( layerProperty.orderId ) + ' ' + layerProperty.name + ' ' + layerProperty.title );
    QString pathName = layerProperty.name.isEmpty() ? QString::number( layerProperty.orderId ) : layerProperty.name;
    QgsWMSLayerItem * layer = new QgsWMSLayerItem( this, layerProperty.title, mPath + '/' + pathName, mCapabilitiesProperty, dataSourceUri, layerProperty );
    //mChildren.append( layer );
    addChildItem( layer );
  }

  mIconName = "mIconWms.svg";

  setState( Populated );
}

QgsWMSLayerItem::~QgsWMSLayerItem()
{
}

QString QgsWMSLayerItem::createUri()
{
  if ( mLayerProperty.name.isEmpty() )
    return ""; // layer collection

  // Number of styles must match number of layers
  mDataSourceUri.setParam( "layers", mLayerProperty.name );
  QString style = !mLayerProperty.style.isEmpty() ? mLayerProperty.style.at( 0 ).name : "";
  mDataSourceUri.setParam( "styles", style );

  QString format;
  // get first supported by qt and server
  QVector<QgsWmsSupportedFormat> formats( QgsWmsProvider::supportedFormats() );
  Q_FOREACH ( const QgsWmsSupportedFormat &f, formats )
  {
    if ( mCapabilitiesProperty.capability.request.getMap.format.indexOf( f.format ) >= 0 )
    {
      format = f.format;
      break;
    }
  }
  mDataSourceUri.setParam( "format", format );

  QString crs;
  // get first known if possible
  QgsCoordinateReferenceSystem testCrs;
  Q_FOREACH ( const QString& c, mLayerProperty.crs )
  {
    testCrs = QgsCRSCache::instance()->crsByOgcWmsCrs( c );
    if ( testCrs.isValid() )
    {
      crs = c;
      break;
    }
  }
  if ( crs.isEmpty() && !mLayerProperty.crs.isEmpty() )
  {
    crs = mLayerProperty.crs[0];
  }
  mDataSourceUri.setParam( "crs", crs );
  //uri = rasterLayerPath + "|layers=" + layers.join( "," ) + "|styles=" + styles.join( "," ) + "|format=" + format + "|crs=" + crs;

  return mDataSourceUri.encodedUri();
}

// ---------------------------------------------------------------------------

QgsWMTSLayerItem::QgsWMTSLayerItem( QgsDataItem *parent,
                                    const QString &name,
                                    const QString &path,
                                    const QgsDataSourceURI &uri,
                                    const QString &id,
                                    const QString &format,
                                    const QString &style,
                                    const QString &tileMatrixSet,
                                    const QString &crs,
                                    const QString &title )
    : QgsLayerItem( parent, name, path, QString(), QgsLayerItem::Raster, "wms" )
    , mDataSourceUri( uri )
    , mId( id )
    , mFormat( format )
    , mStyle( style )
    , mTileMatrixSet( tileMatrixSet )
    , mCrs( crs )
    , mTitle( title )
{
  mUri = createUri();
  setState( Populated );
}

QgsWMTSLayerItem::~QgsWMTSLayerItem()
{
}

QString QgsWMTSLayerItem::createUri()
{
  // TODO dimensions

  QgsDataSourceURI uri( mDataSourceUri );
  uri.setParam( "layers", mId );
  uri.setParam( "styles", mStyle );
  uri.setParam( "format", mFormat );
  uri.setParam( "crs", mCrs );
  uri.setParam( "tileMatrixSet", mTileMatrixSet );
  return uri.encodedUri();
}

// ---------------------------------------------------------------------------
QgsWMSRootItem::QgsWMSRootItem( QgsDataItem* parent, QString name, QString path )
    : QgsDataCollectionItem( parent, name, path )
{
  mCapabilities |= Fast;
  mIconName = "mIconWms.svg";
  populate();
}

QgsWMSRootItem::~QgsWMSRootItem()
{
}

QVector<QgsDataItem*> QgsWMSRootItem::createChildren()
{
  QVector<QgsDataItem*> connections;

  Q_FOREACH ( const QString& connName, QgsWMSConnection::connectionList() )
  {
    QgsWMSConnection connection( connName );
    QgsDataItem * conn = new QgsWMSConnectionItem( this, connName, mPath + '/' + connName, connection.uri().encodedUri() );

    connections.append( conn );
  }
  return connections;
}

QList<QAction*> QgsWMSRootItem::actions()
{
  QList<QAction*> lst;

  QAction* actionNew = new QAction( tr( "New Connection..." ), this );
  connect( actionNew, SIGNAL( triggered() ), this, SLOT( newConnection() ) );
  lst.append( actionNew );

  return lst;
}


QWidget * QgsWMSRootItem::paramWidget()
{
  QgsWMSSourceSelect *select = new QgsWMSSourceSelect( nullptr, nullptr, true, true );
  connect( select, SIGNAL( connectionsChanged() ), this, SLOT( connectionsChanged() ) );
  return select;
}

void QgsWMSRootItem::connectionsChanged()
{
  refresh();
}

void QgsWMSRootItem::newConnection()
{
  QgsNewHttpConnection nc( nullptr );

  if ( nc.exec() )
  {
    refresh();
  }
}


// ---------------------------------------------------------------------------

QGISEXTERN void registerGui( QMainWindow *mainWindow )
{
  QgsTileScaleWidget::showTileScale( mainWindow );
}

QGISEXTERN QgsWMSSourceSelect * selectWidget( QWidget * parent, Qt::WindowFlags fl )
{
  return new QgsWMSSourceSelect( parent, fl );
}


QgsDataItem* QgsWmsDataItemProvider::createDataItem( const QString& thePath, QgsDataItem *parentItem )
{
  QgsDebugMsg( "thePath = " + thePath );
  if ( thePath.isEmpty() )
  {
    return new QgsWMSRootItem( parentItem, "WMS", "wms:" );
  }

  // path schema: wms:/connection name (used by OWS)
  if ( thePath.startsWith( "wms:/" ) )
  {
    QString connectionName = thePath.split( '/' ).last();
    if ( QgsWMSConnection::connectionList().contains( connectionName ) )
    {
      QgsWMSConnection connection( connectionName );
      return new QgsWMSConnectionItem( parentItem, "WMS", thePath, connection.uri().encodedUri() );
    }
  }

  return nullptr;
}

QGISEXTERN QList<QgsDataItemProvider*> dataItemProviders()
{
  return QList<QgsDataItemProvider*>()
         << new QgsWmsDataItemProvider
         << new QgsXyzTileDataItemProvider;
}

// ---------------------------------------------------------------------------


QgsXyzTileRootItem::QgsXyzTileRootItem( QgsDataItem *parent, QString name, QString path )
    : QgsDataCollectionItem( parent, name, path )
{
  mCapabilities |= Fast;
  mIconName = "mIconWms.svg";
  populate();
}

QVector<QgsDataItem *> QgsXyzTileRootItem::createChildren()
{
  QVector<QgsDataItem*> connections;
  Q_FOREACH ( const QString& connName, QgsXyzConnectionUtils::connectionList() )
  {
    QgsXyzConnection connection( QgsXyzConnectionUtils::connection( connName ) );
    QgsDataItem * conn = new QgsXyzLayerItem( this, connName, mPath + '/' + connName, connection.encodedUri() );
    connections.append( conn );
  }
  return connections;
}

QList<QAction *> QgsXyzTileRootItem::actions()
{
  QAction* actionNew = new QAction( tr( "New Connection..." ), this );
  connect( actionNew, SIGNAL( triggered() ), this, SLOT( newConnection() ) );
  return QList<QAction*>() << actionNew;
}

void QgsXyzTileRootItem::newConnection()
{
  QString url = QInputDialog::getText( nullptr, tr( "New XYZ tile layer" ),
                                       tr( "Please enter XYZ tile layer URL. {x}, {y}, {z} will be replaced by actual tile coordinates." ) );
  if ( url.isEmpty() )
    return;

  QString name = QInputDialog::getText( nullptr, tr( "New XYZ tile layer" ),
                                        tr( "Please enter name of the tile layer:" ) );
  if ( name.isEmpty() )
    return;

  QgsXyzConnection conn;
  conn.name = name;
  conn.url = url;
  QgsXyzConnectionUtils::addConnection( conn );

  refresh();
}


// ---------------------------------------------------------------------------


QgsXyzLayerItem::QgsXyzLayerItem( QgsDataItem *parent, QString name, QString path, const QString &encodedUri )
    : QgsLayerItem( parent, name, path, encodedUri, QgsLayerItem::Raster, "wms" )
{
  setState( Populated );
}

QList<QAction *> QgsXyzLayerItem::actions()
{
  QList<QAction*> lst = QgsLayerItem::actions();

  QAction* actionDelete = new QAction( tr( "Delete" ), this );
  connect( actionDelete, SIGNAL( triggered() ), this, SLOT( deleteConnection() ) );
  lst << actionDelete;

  return lst;
}

void QgsXyzLayerItem::deleteConnection()
{
  QgsXyzConnectionUtils::deleteConnection( mName );

  mParent->refresh();
}
