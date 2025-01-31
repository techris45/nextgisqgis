/***************************************************************************
    offline_editing.cpp

    Offline Editing Plugin
    a QGIS plugin
     --------------------------------------
    Date                 : 22-Jul-2010
    Copyright            : (C) 2010 by Sourcepole
    Email                : info at sourcepole.ch
 ***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/


#include "qgsapplication.h"
#include "qgsdatasourceuri.h"
#include "qgsgeometry.h"
#include "qgslayertreegroup.h"
#include "qgslayertreelayer.h"
#include "qgsmaplayer.h"
#include "qgsmaplayerregistry.h"
#include "qgsofflineediting.h"
#include "qgsproject.h"
#include "qgsvectordataprovider.h"
#include "qgsvectorlayereditbuffer.h"
#include "qgsvectorlayerjoinbuffer.h"
#include "qgsslconnect.h"
#include "qgsvisibilitypresetcollection.h"

#include <QDir>
#include <QDomDocument>
#include <QDomNode>
#include <QFile>
#include <QMessageBox>

extern "C"
{
#include <sqlite3.h>
#include <spatialite.h>
}

// TODO: DEBUG
#include <QDebug>
// END

#define CUSTOM_PROPERTY_IS_OFFLINE_EDITABLE "isOfflineEditable"
#define CUSTOM_PROPERTY_REMOTE_SOURCE "remoteSource"
#define CUSTOM_PROPERTY_REMOTE_PROVIDER "remoteProvider"
#define PROJECT_ENTRY_SCOPE_OFFLINE "OfflineEditingPlugin"
#define PROJECT_ENTRY_KEY_OFFLINE_DB_PATH "/OfflineDbPath"

QgsOfflineEditing::QgsOfflineEditing():
    syncError( false )
{
  connect( QgsMapLayerRegistry::instance(), SIGNAL( layerWasAdded( QgsMapLayer* ) ), this, SLOT( layerAdded( QgsMapLayer* ) ) );
}

QgsOfflineEditing::~QgsOfflineEditing()
{
}

/**
 * convert current project to offline project
 * returns offline project file path
 *
 * Workflow:
 *  - copy layers to spatialite
 *  - create spatialite db at offlineDataPath
 *  - create table for each layer
 *  - add new spatialite layer
 *  - copy features
 *  - save as offline project
 *  - mark offline layers
 *  - remove remote layers
 *  - mark as offline project
 */
bool QgsOfflineEditing::convertToOfflineProject( const QString& offlineDataPath, const QString& offlineDbFile, const QStringList& layerIds, bool onlySelected )
{
  if ( layerIds.isEmpty() )
  {
    return false;
  }
  QString dbPath = QDir( offlineDataPath ).absoluteFilePath( offlineDbFile );
  if ( createSpatialiteDB( dbPath ) )
  {
    sqlite3* db;
    int rc = QgsSLConnect::sqlite3_open( dbPath.toUtf8().constData(), &db );
    if ( rc != SQLITE_OK )
    {
      showWarning( tr( "Could not open the spatialite database" ) );
    }
    else
    {
      // create logging tables
      createLoggingTables( db );

      emit progressStarted();

      QMap<QString, QgsVectorJoinList > joinInfoBuffer;
      QMap<QString, QgsVectorLayer*> layerIdMapping;

      Q_FOREACH ( const QString& layerId, layerIds )
      {
        QgsMapLayer* layer = QgsMapLayerRegistry::instance()->mapLayer( layerId );
        QgsVectorLayer* vl = qobject_cast<QgsVectorLayer*>( layer );
        if ( !vl )
          continue;
        QgsVectorJoinList joins = vl->vectorJoins();

        // Layer names will be appended an _offline suffix
        // Join fields are prefixed with the layer name and we do not want the
        // field name to change so we stabilize the field name by defining a
        // custom prefix with the layername without _offline suffix.
        QgsVectorJoinList::iterator joinIt = joins.begin();
        while ( joinIt != joins.end() )
        {
          if ( joinIt->prefix.isNull() )
          {
            QgsVectorLayer* vl = qobject_cast<QgsVectorLayer*>( QgsMapLayerRegistry::instance()->mapLayer( joinIt->joinLayerId ) );

            if ( vl )
              joinIt->prefix = vl->name() + '_';
          }
          ++joinIt;
        }
        joinInfoBuffer.insert( vl->id(), joins );
      }

      // copy selected vector layers to SpatiaLite
      for ( int i = 0; i < layerIds.count(); i++ )
      {
        emit layerProgressUpdated( i + 1, layerIds.count() );

        QgsMapLayer* layer = QgsMapLayerRegistry::instance()->mapLayer( layerIds.at( i ) );
        QgsVectorLayer* vl = qobject_cast<QgsVectorLayer*>( layer );
        if ( vl )
        {
          QString origLayerId = vl->id();
          QgsVectorLayer* newLayer = copyVectorLayer( vl, db, dbPath, onlySelected );

          if ( newLayer )
          {
            layerIdMapping.insert( origLayerId, newLayer );
            // remove remote layer
            QgsMapLayerRegistry::instance()->removeMapLayers(
              QStringList() << origLayerId );
          }
        }
      }

      // restore join info on new spatialite layer
      QMap<QString, QgsVectorJoinList >::ConstIterator it;
      for ( it = joinInfoBuffer.constBegin(); it != joinInfoBuffer.constEnd(); ++it )
      {
        QgsVectorLayer* newLayer = layerIdMapping.value( it.key() );

        if ( newLayer )
        {
          Q_FOREACH ( QgsVectorJoinInfo join, it.value() )
          {
            QgsVectorLayer* newJoinedLayer = layerIdMapping.value( join.joinLayerId );
            if ( newJoinedLayer )
            {
              // If the layer has been offline'd, update join information
              join.joinLayerId = newJoinedLayer->id();
            }
            newLayer->addJoin( join );
          }
        }
      }


      emit progressStopped();

      QgsSLConnect::sqlite3_close( db );

      // save offline project
      QString projectTitle = QgsProject::instance()->title();
      if ( projectTitle.isEmpty() )
      {
        projectTitle = QFileInfo( QgsProject::instance()->fileName() ).fileName();
      }
      projectTitle += " (offline)";
      QgsProject::instance()->setTitle( projectTitle );

      QgsProject::instance()->writeEntry( PROJECT_ENTRY_SCOPE_OFFLINE, PROJECT_ENTRY_KEY_OFFLINE_DB_PATH, QgsProject::instance()->writePath( dbPath ) );

      return true;
    }
  }

  return false;
}

bool QgsOfflineEditing::isOfflineProject() const
{
  return !QgsProject::instance()->readEntry( PROJECT_ENTRY_SCOPE_OFFLINE, PROJECT_ENTRY_KEY_OFFLINE_DB_PATH ).isEmpty();
}

void QgsOfflineEditing::synchronize()
{
  // open logging db
  sqlite3* db = openLoggingDb();
  if ( !db )
  {
    return;
  }

  emit progressStarted();

  // restore and sync remote layers
  QList<QgsMapLayer*> offlineLayers;
  QMap<QString, QgsMapLayer*> mapLayers = QgsMapLayerRegistry::instance()->mapLayers();
  for ( QMap<QString, QgsMapLayer*>::iterator layer_it = mapLayers.begin() ; layer_it != mapLayers.end(); ++layer_it )
  {
    QgsMapLayer* layer = layer_it.value();
    if ( layer->customProperty( CUSTOM_PROPERTY_IS_OFFLINE_EDITABLE, false ).toBool() )
    {
      offlineLayers << layer;
    }
  }

  QgsDebugMsgLevel( QString( "Found %1 offline layers" ).arg( offlineLayers.count() ), 4 );
  for ( int l = 0; l < offlineLayers.count(); l++ )
  {
    QgsMapLayer* layer = offlineLayers.at( l );

    emit layerProgressUpdated( l + 1, offlineLayers.count() );

    QString remoteSource = layer->customProperty( CUSTOM_PROPERTY_REMOTE_SOURCE, "" ).toString();
    QString remoteProvider = layer->customProperty( CUSTOM_PROPERTY_REMOTE_PROVIDER, "" ).toString();
    QString remoteName = layer->name();
    remoteName.remove( QRegExp( " \\(offline\\)$" ) );

    QgsVectorLayer* remoteLayer = new QgsVectorLayer( remoteSource, remoteName, remoteProvider );
    if ( remoteLayer->isValid() )
    {
      // Rebuild WFS cache to get feature id<->GML fid mapping
      if ( remoteLayer->dataProvider()->name().contains( "WFS", Qt::CaseInsensitive ) )
      {
        QgsFeatureIterator fit = remoteLayer->getFeatures();
        QgsFeature f;
        while ( fit.nextFeature( f ) )
        {
        }
      }
      // TODO: only add remote layer if there are log entries?

      QgsVectorLayer* offlineLayer = qobject_cast<QgsVectorLayer*>( layer );

      // register this layer with the central layers registry
      QgsMapLayerRegistry::instance()->addMapLayers( QList<QgsMapLayer *>() << remoteLayer, true );

      // copy style
      copySymbology( offlineLayer, remoteLayer );
      updateRelations( offlineLayer, remoteLayer );
      updateVisibilityPresets( offlineLayer, remoteLayer );

      // apply layer edit log
      syncError = false;
      QString qgisLayerId = layer->id();
      QString sql = QString( "SELECT \"id\" FROM 'log_layer_ids' WHERE \"qgis_id\" = '%1'" ).arg( qgisLayerId );
      int layerId = sqlQueryInt( db, sql, -1 );
      if ( layerId != -1 )
      {
        remoteLayer->startEditing();

        // TODO: only get commitNos of this layer?
        int commitNo = getCommitNo( db );
        QgsDebugMsgLevel( QString( "Found %1 commits" ).arg( commitNo ), 4 );
        for ( int i = 0; i < commitNo; i++ )
        {
          QgsDebugMsgLevel( "Apply commits chronologically", 4 );
          // apply commits chronologically
          applyAttributesAdded( remoteLayer, db, layerId, i );
          if ( !syncError )
            applyAttributeValueChanges( offlineLayer, remoteLayer, db, layerId, i );
          if ( !syncError )
            applyGeometryChanges( remoteLayer, db, layerId, i );
        }

        if ( !syncError )
          applyFeaturesAdded( offlineLayer, remoteLayer, db, layerId );
        if ( !syncError )
          applyFeaturesRemoved( remoteLayer, db, layerId );

        if ( !syncError )
        {
          if ( remoteLayer->commitChanges() )
          {
            // update fid lookup
            updateFidLookup( remoteLayer, db, layerId );

            // clear edit log for this layer
            sql = QString( "DELETE FROM 'log_added_attrs' WHERE \"layer_id\" = %1" ).arg( layerId );
            sqlExec( db, sql );
            sql = QString( "DELETE FROM 'log_added_features' WHERE \"layer_id\" = %1" ).arg( layerId );
            sqlExec( db, sql );
            sql = QString( "DELETE FROM 'log_removed_features' WHERE \"layer_id\" = %1" ).arg( layerId );
            sqlExec( db, sql );
            sql = QString( "DELETE FROM 'log_feature_updates' WHERE \"layer_id\" = %1" ).arg( layerId );
            sqlExec( db, sql );
            sql = QString( "DELETE FROM 'log_geometry_updates' WHERE \"layer_id\" = %1" ).arg( layerId );
            sqlExec( db, sql );
          }
          else
          {
            showWarning( remoteLayer->commitErrors().join( "\n" ) );
          }
        }
        else
        {
          remoteLayer->rollBack();
          showWarning( tr( "Synchronization failed" ) );
        }
      }
      else
      {
        QgsDebugMsg( "Could not find the layer id in the edit logs!" );
      }
      // Invalidate the connection to force a reload if the project is put offline
      // again with the same path
      offlineLayer->dataProvider()->invalidateConnections( QgsDataSourceURI( offlineLayer->source() ).database() );
      // remove offline layer
      QgsMapLayerRegistry::instance()->removeMapLayers( QStringList() << qgisLayerId );


      // disable offline project
      QString projectTitle = QgsProject::instance()->title();
      projectTitle.remove( QRegExp( " \\(offline\\)$" ) );
      QgsProject::instance()->setTitle( projectTitle );
      QgsProject::instance()->removeEntry( PROJECT_ENTRY_SCOPE_OFFLINE, PROJECT_ENTRY_KEY_OFFLINE_DB_PATH );
      remoteLayer->reload(); //update with other changes
    }
    else
    {
      QgsDebugMsg( "Remote layer is not valid!" );
    }
  }

  // reset commitNo
  QString sql = QString( "UPDATE 'log_indices' SET 'last_index' = 0 WHERE \"name\" = 'commit_no'" );
  sqlExec( db, sql );

  emit progressStopped();

  sqlite3_close( db );
}

void QgsOfflineEditing::initializeSpatialMetadata( sqlite3 *sqlite_handle )
{
  // attempting to perform self-initialization for a newly created DB
  if ( !sqlite_handle )
    return;
  // checking if this DB is really empty
  char **results;
  int rows, columns;
  int ret = sqlite3_get_table( sqlite_handle, "select count(*) from sqlite_master", &results, &rows, &columns, nullptr );
  if ( ret != SQLITE_OK )
    return;
  int count = 0;
  if ( rows >= 1 )
  {
    for ( int i = 1; i <= rows; i++ )
      count = atoi( results[( i * columns ) + 0] );
  }

  sqlite3_free_table( results );

  if ( count > 0 )
    return;

  bool above41 = false;
  ret = sqlite3_get_table( sqlite_handle, "select spatialite_version()", &results, &rows, &columns, nullptr );
  if ( ret == SQLITE_OK && rows == 1 && columns == 1 )
  {
    QString version = QString::fromUtf8( results[1] );
    QStringList parts = version.split( ' ', QString::SkipEmptyParts );
    if ( parts.size() >= 1 )
    {
      QStringList verparts = parts[0].split( '.', QString::SkipEmptyParts );
      above41 = verparts.size() >= 2 && ( verparts[0].toInt() > 4 || ( verparts[0].toInt() == 4 && verparts[1].toInt() >= 1 ) );
    }
  }

  sqlite3_free_table( results );

  // all right, it's empty: proceding to initialize
  char *errMsg = nullptr;
  ret = sqlite3_exec( sqlite_handle, above41 ? "SELECT InitSpatialMetadata(1)" : "SELECT InitSpatialMetadata()", nullptr, nullptr, &errMsg );

  if ( ret != SQLITE_OK )
  {
    QString errCause = tr( "Unable to initialize SpatialMetadata:\n" );
    errCause += QString::fromUtf8( errMsg );
    showWarning( errCause );
    sqlite3_free( errMsg );
    return;
  }
  spatial_ref_sys_init( sqlite_handle, 0 );
}

bool QgsOfflineEditing::createSpatialiteDB( const QString& offlineDbPath )
{
  int ret;
  sqlite3 *sqlite_handle;
  char *errMsg = nullptr;
  QFile newDb( offlineDbPath );
  if ( newDb.exists() )
  {
    QFile::remove( offlineDbPath );
  }

  // see also QgsNewSpatialiteLayerDialog::createDb()

  QFileInfo fullPath = QFileInfo( offlineDbPath );
  QDir path = fullPath.dir();

  // Must be sure there is destination directory ~/.qgis
  QDir().mkpath( path.absolutePath() );

  // creating/opening the new database
  QString dbPath = newDb.fileName();
  ret = QgsSLConnect::sqlite3_open_v2( dbPath.toUtf8().constData(), &sqlite_handle, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr );
  if ( ret )
  {
    // an error occurred
    QString errCause = tr( "Could not create a new database\n" );
    errCause += QString::fromUtf8( sqlite3_errmsg( sqlite_handle ) );
    sqlite3_close( sqlite_handle );
    showWarning( errCause );
    return false;
  }
  // activating Foreign Key constraints
  ret = sqlite3_exec( sqlite_handle, "PRAGMA foreign_keys = 1", nullptr, nullptr, &errMsg );
  if ( ret != SQLITE_OK )
  {
    showWarning( tr( "Unable to activate FOREIGN_KEY constraints" ) );
    sqlite3_free( errMsg );
    QgsSLConnect::sqlite3_close( sqlite_handle );
    return false;
  }
  initializeSpatialMetadata( sqlite_handle );

  // all done: closing the DB connection
  QgsSLConnect::sqlite3_close( sqlite_handle );

  return true;
}

void QgsOfflineEditing::createLoggingTables( sqlite3* db )
{
  // indices
  QString sql = "CREATE TABLE 'log_indices' ('name' TEXT, 'last_index' INTEGER)";
  sqlExec( db, sql );

  sql = "INSERT INTO 'log_indices' VALUES ('commit_no', 0)";
  sqlExec( db, sql );

  sql = "INSERT INTO 'log_indices' VALUES ('layer_id', 0)";
  sqlExec( db, sql );

  // layername <-> layer id
  sql = "CREATE TABLE 'log_layer_ids' ('id' INTEGER, 'qgis_id' TEXT)";
  sqlExec( db, sql );

  // offline fid <-> remote fid
  sql = "CREATE TABLE 'log_fids' ('layer_id' INTEGER, 'offline_fid' INTEGER, 'remote_fid' INTEGER)";
  sqlExec( db, sql );

  // added attributes
  sql = "CREATE TABLE 'log_added_attrs' ('layer_id' INTEGER, 'commit_no' INTEGER, ";
  sql += "'name' TEXT, 'type' INTEGER, 'length' INTEGER, 'precision' INTEGER, 'comment' TEXT)";
  sqlExec( db, sql );

  // added features
  sql = "CREATE TABLE 'log_added_features' ('layer_id' INTEGER, 'fid' INTEGER)";
  sqlExec( db, sql );

  // removed features
  sql = "CREATE TABLE 'log_removed_features' ('layer_id' INTEGER, 'fid' INTEGER)";
  sqlExec( db, sql );

  // feature updates
  sql = "CREATE TABLE 'log_feature_updates' ('layer_id' INTEGER, 'commit_no' INTEGER, 'fid' INTEGER, 'attr' INTEGER, 'value' TEXT)";
  sqlExec( db, sql );

  // geometry updates
  sql = "CREATE TABLE 'log_geometry_updates' ('layer_id' INTEGER, 'commit_no' INTEGER, 'fid' INTEGER, 'geom_wkt' TEXT)";
  sqlExec( db, sql );

  /* TODO: other logging tables
    - attr delete (not supported by SpatiaLite provider)
  */
}

QgsVectorLayer* QgsOfflineEditing::copyVectorLayer( QgsVectorLayer* layer, sqlite3* db, const QString& offlineDbPath , bool onlySelected )
{
  if ( !layer )
    return nullptr;

  QString tableName = layer->id();
  QgsDebugMsgLevel( QString( "Creating offline table %1 ..." ).arg( tableName ), 4 );

  // create table
  QString sql = QString( "CREATE TABLE '%1' (" ).arg( tableName );
  QString delim = "";
  Q_FOREACH ( const QgsField& field, layer->dataProvider()->fields() )
  {
    QString dataType = "";
    QVariant::Type type = field.type();
    if ( type == QVariant::Int || type == QVariant::LongLong )
    {
      dataType = "INTEGER";
    }
    else if ( type == QVariant::Double )
    {
      dataType = "REAL";
    }
    else if ( type == QVariant::String )
    {
      dataType = "TEXT";
    }
    else
    {
      showWarning( tr( "%1: Unknown data type %2. Not using type affinity for the field." ).arg( field.name(), QVariant::typeToName( type ) ) );
    }

    sql += delim + QString( "'%1' %2" ).arg( field.name(), dataType );
    delim = ',';
  }
  sql += ')';

  int rc = sqlExec( db, sql );

  // add geometry column
  if ( layer->hasGeometryType() )
  {
    QString geomType = "";
    switch ( QGis::flatType( layer->wkbType() ) )
    {
      case QGis::WKBPoint:
        geomType = "POINT";
        break;
      case QGis::WKBMultiPoint:
        geomType = "MULTIPOINT";
        break;
      case QGis::WKBLineString:
        geomType = "LINESTRING";
        break;
      case QGis::WKBMultiLineString:
        geomType = "MULTILINESTRING";
        break;
      case QGis::WKBPolygon:
        geomType = "POLYGON";
        break;
      case QGis::WKBMultiPolygon:
        geomType = "MULTIPOLYGON";
        break;
      default:
        showWarning( tr( "QGIS wkbType %1 not supported" ).arg( layer->wkbType() ) );
        break;
    };

    if ( QGis::flatType( layer->wkbType() ) != layer->wkbType() )
      showWarning( tr( "Will drop Z and M values from layer %1 in offline copy." ).arg( layer->name() ) );

    QString sqlAddGeom = QString( "SELECT AddGeometryColumn('%1', 'Geometry', %2, '%3', 2)" )
                         .arg( tableName )
                         .arg( layer->crs().authid().startsWith( "EPSG:", Qt::CaseInsensitive ) ? layer->crs().authid().mid( 5 ).toLong() : 0 )
                         .arg( geomType );

    // create spatial index
    QString sqlCreateIndex = QString( "SELECT CreateSpatialIndex('%1', 'Geometry')" ).arg( tableName );

    if ( rc == SQLITE_OK )
    {
      rc = sqlExec( db, sqlAddGeom );
      if ( rc == SQLITE_OK )
      {
        rc = sqlExec( db, sqlCreateIndex );
      }
    }
  }

  if ( rc == SQLITE_OK )
  {
    // add new layer
    QString connectionString = QString( "dbname='%1' table='%2'%3 sql=" )
                               .arg( offlineDbPath,
                                     tableName, layer->hasGeometryType() ? "(Geometry)" : "" );
    QgsVectorLayer* newLayer = new QgsVectorLayer( connectionString,
        layer->name() + " (offline)", "spatialite" );
    if ( newLayer->isValid() )
    {
      // mark as offline layer
      newLayer->setCustomProperty( CUSTOM_PROPERTY_IS_OFFLINE_EDITABLE, true );

      // store original layer source
      newLayer->setCustomProperty( CUSTOM_PROPERTY_REMOTE_SOURCE, layer->source() );
      newLayer->setCustomProperty( CUSTOM_PROPERTY_REMOTE_PROVIDER, layer->providerType() );

      // register this layer with the central layers registry
      QgsMapLayerRegistry::instance()->addMapLayers(
        QList<QgsMapLayer *>() << newLayer );

      // copy style
      Q_NOWARN_DEPRECATED_PUSH
      bool hasLabels = layer->hasLabelsEnabled();
      Q_NOWARN_DEPRECATED_POP
      if ( !hasLabels )
      {
        // NOTE: copy symbology before adding the layer so it is displayed correctly
        copySymbology( layer, newLayer );
      }

      QgsLayerTreeGroup* layerTreeRoot = QgsProject::instance()->layerTreeRoot();
      // Find the parent group of the original layer
      QgsLayerTreeLayer* layerTreeLayer = layerTreeRoot->findLayer( layer->id() );
      if ( layerTreeLayer )
      {
        QgsLayerTreeGroup* parentTreeGroup = qobject_cast<QgsLayerTreeGroup*>( layerTreeLayer->parent() );
        if ( parentTreeGroup )
        {
          int index = parentTreeGroup->children().indexOf( layerTreeLayer );
          // Move the new layer from the root group to the new group
          QgsLayerTreeLayer* newLayerTreeLayer = layerTreeRoot->findLayer( newLayer->id() );
          if ( newLayerTreeLayer )
          {
            QgsLayerTreeNode* newLayerTreeLayerClone = newLayerTreeLayer->clone();
            QgsLayerTreeGroup* grp = qobject_cast<QgsLayerTreeGroup*>( newLayerTreeLayer->parent() );
            parentTreeGroup->insertChildNode( index, newLayerTreeLayerClone );
            if ( grp )
              grp->removeChildNode( newLayerTreeLayer );
          }
        }
      }

      if ( hasLabels )
      {
        // NOTE: copy symbology of layers with labels enabled after adding to project, as it will crash otherwise (WORKAROUND)
        copySymbology( layer, newLayer );
      }

      updateRelations( layer, newLayer );
      updateVisibilityPresets( layer, newLayer );
      // copy features
      newLayer->startEditing();
      QgsFeature f;

      QgsFeatureRequest req;

      if ( onlySelected )
      {
        QgsFeatureIds selectedFids = layer->selectedFeaturesIds();
        if ( !selectedFids.isEmpty() )
          req.setFilterFids( selectedFids );
      }

      // NOTE: force feature recount for PostGIS layer, else only visible features are counted, before iterating over all features (WORKAROUND)
      layer->setSubsetString( layer->subsetString() );

      QgsFeatureIterator fit = layer->dataProvider()->getFeatures( req );

      if ( req.filterType() == QgsFeatureRequest::FilterFids )
      {
        emit progressModeSet( QgsOfflineEditing::CopyFeatures, layer->selectedFeaturesIds().size() );
      }
      else
      {
        emit progressModeSet( QgsOfflineEditing::CopyFeatures, layer->dataProvider()->featureCount() );
      }
      int featureCount = 1;

      QList<QgsFeatureId> remoteFeatureIds;
      while ( fit.nextFeature( f ) )
      {
        remoteFeatureIds << f.id();

        // NOTE: Spatialite provider ignores position of geometry column
        // fill gap in QgsAttributeMap if geometry column is not last (WORKAROUND)
        int column = 0;
        QgsAttributes attrs = f.attributes();
        QgsAttributes newAttrs( attrs.count() );
        for ( int it = 0; it < attrs.count(); ++it )
        {
          newAttrs[column++] = attrs.at( it );
        }
        f.setAttributes( newAttrs );

        // The spatialite provider doesn't properly handle Z and M values
        if ( f.geometry() && f.geometry()->geometry() )
        {
          QgsAbstractGeometryV2* geom = f.geometry()->geometry()->clone();
          geom->dropZValue();
          geom->dropMValue();
          f.geometry()->setGeometry( geom );
        }

        if ( !newLayer->addFeature( f, false ) )
          return nullptr;

        emit progressUpdated( featureCount++ );
      }
      if ( newLayer->commitChanges() )
      {
        emit progressModeSet( QgsOfflineEditing::ProcessFeatures, layer->dataProvider()->featureCount() );
        featureCount = 1;

        // update feature id lookup
        int layerId = getOrCreateLayerId( db, newLayer->id() );
        QList<QgsFeatureId> offlineFeatureIds;

        QgsFeatureIterator fit = newLayer->getFeatures( QgsFeatureRequest().setFlags( QgsFeatureRequest::NoGeometry ).setSubsetOfAttributes( QgsAttributeList() ) );
        while ( fit.nextFeature( f ) )
        {
          offlineFeatureIds << f.id();
        }

        // NOTE: insert fids in this loop, as the db is locked during newLayer->nextFeature()
        sqlExec( db, "BEGIN" );
        int remoteCount = remoteFeatureIds.size();
        for ( int i = 0; i < remoteCount; i++ )
        {
          // Check if the online feature has been fetched (WFS download aborted for some reason)
          if ( i < offlineFeatureIds.count() )
          {
            addFidLookup( db, layerId, offlineFeatureIds.at( i ), remoteFeatureIds.at( i ) );
          }
          else
          {
            showWarning( tr( "Feature cannot be copied to the offline layer, please check if the online layer '%1' is still accessible." ).arg( layer->name() ) );
            return nullptr;
          }
          emit progressUpdated( featureCount++ );
        }
        sqlExec( db, "COMMIT" );
      }
      else
      {
        showWarning( newLayer->commitErrors().join( "\n" ) );
      }
    }
    return newLayer;
  }
  return nullptr;
}

void QgsOfflineEditing::applyAttributesAdded( QgsVectorLayer* remoteLayer, sqlite3* db, int layerId, int commitNo )
{
  QString sql = QString( "SELECT \"name\", \"type\", \"length\", \"precision\", \"comment\" FROM 'log_added_attrs' WHERE \"layer_id\" = %1 AND \"commit_no\" = %2" ).arg( layerId ).arg( commitNo );
  QList<QgsField> fields = sqlQueryAttributesAdded( db, sql );

  const QgsVectorDataProvider* provider = remoteLayer->dataProvider();
  QList<QgsVectorDataProvider::NativeType> nativeTypes = provider->nativeTypes();

  // NOTE: uses last matching QVariant::Type of nativeTypes
  QMap < QVariant::Type, QString /*typeName*/ > typeNameLookup;
  for ( int i = 0; i < nativeTypes.size(); i++ )
  {
    QgsVectorDataProvider::NativeType nativeType = nativeTypes.at( i );
    typeNameLookup[ nativeType.mType ] = nativeType.mTypeName;
  }

  emit progressModeSet( QgsOfflineEditing::AddFields, fields.size() );

  for ( int i = 0; i < fields.size(); i++ )
  {
    // lookup typename from layer provider
    QgsField field = fields[i];
    if ( typeNameLookup.contains( field.type() ) )
    {
      QString typeName = typeNameLookup[ field.type()];
      field.setTypeName( typeName );
      if ( !remoteLayer->addAttribute( field ) )
      {
        syncError = true;
        return;
      }
    }
    else
    {
      showWarning( QString( "Could not add attribute '%1' of type %2" ).arg( field.name() ).arg( field.type() ) );
    }

    emit progressUpdated( i + 1 );
  }
}

void QgsOfflineEditing::applyFeaturesAdded( QgsVectorLayer* offlineLayer, QgsVectorLayer* remoteLayer, sqlite3* db, int layerId )
{
  QString sql = QString( "SELECT \"fid\" FROM 'log_added_features' WHERE \"layer_id\" = %1" ).arg( layerId );
  QList<int> newFeatureIds = sqlQueryInts( db, sql );

  QgsFields remoteFlds = remoteLayer->fields();

  QgsExpressionContext context;
  context << QgsExpressionContextUtils::globalScope()
  << QgsExpressionContextUtils::projectScope()
  << QgsExpressionContextUtils::layerScope( remoteLayer );

  // get new features from offline layer
  QgsFeatureList features;
  for ( int i = 0; i < newFeatureIds.size(); i++ )
  {
    QgsFeature feature;
    if ( offlineLayer->getFeatures( QgsFeatureRequest().setFilterFid( newFeatureIds.at( i ) ) ).nextFeature( feature ) )
    {
      features << feature;
    }
  }

  // copy features to remote layer
  emit progressModeSet( QgsOfflineEditing::AddFeatures, features.size() );

  int i = 1;
  int newAttrsCount = remoteLayer->fields().count();
  for ( QgsFeatureList::iterator it = features.begin(); it != features.end(); ++it )
  {
    QgsFeature f = *it;

    // NOTE: Spatialite provider ignores position of geometry column
    // restore gap in QgsAttributeMap if geometry column is not last (WORKAROUND)
    QMap<int, int> attrLookup = attributeLookup( offlineLayer, remoteLayer );
    QgsAttributes newAttrs( newAttrsCount );
    QgsAttributes attrs = f.attributes();
    for ( int it = 0; it < attrs.count(); ++it )
    {
      newAttrs[ attrLookup[ it ] ] = attrs.at( it );
    }

    // try to use default value from the provider
    // (important especially e.g. for postgis primary key generated from a sequence)
    for ( int k = 0; k < newAttrs.count(); ++k )
    {
      if ( !newAttrs.at( k ).isNull() )
        continue;

      if ( !remoteLayer->defaultValueExpression( k ).isEmpty() )
        newAttrs[k] = remoteLayer->defaultValue( k, f, &context );
      else if ( remoteFlds.fieldOrigin( k ) == QgsFields::OriginProvider )
        newAttrs[k] = remoteLayer->dataProvider()->defaultValue( remoteFlds.fieldOriginIndex( k ) );
    }

    f.setAttributes( newAttrs );

    if ( !remoteLayer->addFeature( f, false ) )
    {
      syncError = true;
      return;
    }

    emit progressUpdated( i++ );
  }
}

void QgsOfflineEditing::applyFeaturesRemoved( QgsVectorLayer* remoteLayer, sqlite3* db, int layerId )
{
  QString sql = QString( "SELECT \"fid\" FROM 'log_removed_features' WHERE \"layer_id\" = %1" ).arg( layerId );
  QgsFeatureIds values = sqlQueryFeaturesRemoved( db, sql );

  emit progressModeSet( QgsOfflineEditing::RemoveFeatures, values.size() );

  int i = 1;
  for ( QgsFeatureIds::const_iterator it = values.begin(); it != values.end(); ++it )
  {
    QgsFeatureId fid = remoteFid( db, layerId, *it );
    if ( !remoteLayer->deleteFeature( fid ) )
    {
      syncError = true;
      return;
    }

    emit progressUpdated( i++ );
  }
}

void QgsOfflineEditing::applyAttributeValueChanges( QgsVectorLayer* offlineLayer, QgsVectorLayer* remoteLayer, sqlite3* db, int layerId, int commitNo )
{
  QString sql = QString( "SELECT \"fid\", \"attr\", \"value\" FROM 'log_feature_updates' WHERE \"layer_id\" = %1 AND \"commit_no\" = %2 " ).arg( layerId ).arg( commitNo );
  AttributeValueChanges values = sqlQueryAttributeValueChanges( db, sql );

  emit progressModeSet( QgsOfflineEditing::UpdateFeatures, values.size() );

  QMap<int, int> attrLookup = attributeLookup( offlineLayer, remoteLayer );

  for ( int i = 0; i < values.size(); i++ )
  {
    QgsFeatureId fid = remoteFid( db, layerId, values.at( i ).fid );
    QgsDebugMsgLevel( QString( "Offline changeAttributeValue %1 = %2" ).arg( QString( attrLookup[ values.at( i ).attr ] ), values.at( i ).value ), 4 );
    if ( !remoteLayer->changeAttributeValue( fid, attrLookup[ values.at( i ).attr ], values.at( i ).value ) )
    {
      syncError = true;
      return;
    }

    emit progressUpdated( i + 1 );
  }
}

void QgsOfflineEditing::applyGeometryChanges( QgsVectorLayer* remoteLayer, sqlite3* db, int layerId, int commitNo )
{
  QString sql = QString( "SELECT \"fid\", \"geom_wkt\" FROM 'log_geometry_updates' WHERE \"layer_id\" = %1 AND \"commit_no\" = %2" ).arg( layerId ).arg( commitNo );
  GeometryChanges values = sqlQueryGeometryChanges( db, sql );

  emit progressModeSet( QgsOfflineEditing::UpdateGeometries, values.size() );

  for ( int i = 0; i < values.size(); i++ )
  {
    QgsFeatureId fid = remoteFid( db, layerId, values.at( i ).fid );
    if ( !remoteLayer->changeGeometry( fid, QgsGeometry::fromWkt( values.at( i ).geom_wkt ) ) )
    {
      syncError = true;
      return;
    }

    emit progressUpdated( i + 1 );
  }
}

void QgsOfflineEditing::updateFidLookup( QgsVectorLayer* remoteLayer, sqlite3* db, int layerId )
{
  // update fid lookup for added features

  // get remote added fids
  // NOTE: use QMap for sorted fids
  QMap < QgsFeatureId, bool /*dummy*/ > newRemoteFids;
  QgsFeature f;

  QgsFeatureIterator fit = remoteLayer->getFeatures( QgsFeatureRequest().setFlags( QgsFeatureRequest::NoGeometry ).setSubsetOfAttributes( QgsAttributeList() ) );

  emit progressModeSet( QgsOfflineEditing::ProcessFeatures, remoteLayer->featureCount() );

  int i = 1;
  while ( fit.nextFeature( f ) )
  {
    if ( offlineFid( db, layerId, f.id() ) == -1 )
    {
      newRemoteFids[ f.id()] = true;
    }

    emit progressUpdated( i++ );
  }

  // get local added fids
  // NOTE: fids are sorted
  QString sql = QString( "SELECT \"fid\" FROM 'log_added_features' WHERE \"layer_id\" = %1" ).arg( layerId );
  QList<int> newOfflineFids = sqlQueryInts( db, sql );

  if ( newRemoteFids.size() != newOfflineFids.size() )
  {
    //showWarning( QString( "Different number of new features on offline layer (%1) and remote layer (%2)" ).arg(newOfflineFids.size()).arg(newRemoteFids.size()) );
  }
  else
  {
    // add new fid lookups
    i = 0;
    sqlExec( db, "BEGIN" );
    for ( QMap<QgsFeatureId, bool>::const_iterator it = newRemoteFids.begin(); it != newRemoteFids.end(); ++it )
    {
      addFidLookup( db, layerId, newOfflineFids.at( i++ ), it.key() );
    }
    sqlExec( db, "COMMIT" );
  }
}

void QgsOfflineEditing::copySymbology( QgsVectorLayer* sourceLayer, QgsVectorLayer* targetLayer )
{
  QString error;
  QDomDocument doc;
  sourceLayer->exportNamedStyle( doc, error );

  if ( error.isEmpty() )
  {
    targetLayer->importNamedStyle( doc, error );
  }
  if ( !error.isEmpty() )
  {
    showWarning( error );
  }
}

void QgsOfflineEditing::updateRelations( QgsVectorLayer* sourceLayer, QgsVectorLayer* targetLayer )
{
  QgsRelationManager* relationManager = QgsProject::instance()->relationManager();
  QList<QgsRelation> relations;
  relations = relationManager->referencedRelations( sourceLayer );

  Q_FOREACH ( QgsRelation relation, relations )
  {
    relationManager->removeRelation( relation );
    relation.setReferencedLayer( targetLayer->id() );
    relationManager->addRelation( relation );
  }

  relations = relationManager->referencingRelations( sourceLayer );

  Q_FOREACH ( QgsRelation relation, relations )
  {
    relationManager->removeRelation( relation );
    relation.setReferencingLayer( targetLayer->id() );
    relationManager->addRelation( relation );
  }
}

void QgsOfflineEditing::updateVisibilityPresets( QgsVectorLayer* sourceLayer, QgsVectorLayer* targetLayer )
{
  QgsVisibilityPresetCollection* presetCollection = QgsProject::instance()->visibilityPresetCollection();
  QStringList visibilityPresets = presetCollection->presets();

  Q_FOREACH ( const QString& preset, visibilityPresets )
  {
    QgsVisibilityPresetCollection::PresetRecord record = presetCollection->presetState( preset );

    if ( record.mVisibleLayerIDs.removeOne( sourceLayer->id() ) )
      record.mVisibleLayerIDs.append( targetLayer->id() );

    QString style = record.mPerLayerCurrentStyle.value( sourceLayer->id() );
    if ( !style.isNull() )
    {
      record.mPerLayerCurrentStyle.remove( sourceLayer->id() );
      record.mPerLayerCurrentStyle.insert( targetLayer->id(), style );
    }

    if ( !record.mPerLayerCheckedLegendSymbols.contains( sourceLayer->id() ) )
    {
      QSet<QString> checkedSymbols = record.mPerLayerCheckedLegendSymbols.value( sourceLayer->id() );
      record.mPerLayerCheckedLegendSymbols.remove( sourceLayer->id() );
      record.mPerLayerCheckedLegendSymbols.insert( targetLayer->id(), checkedSymbols );
    }

    QgsProject::instance()->visibilityPresetCollection()->update( preset, record );
  }
}

// NOTE: use this to map column indices in case the remote geometry column is not last
QMap<int, int> QgsOfflineEditing::attributeLookup( QgsVectorLayer* offlineLayer, QgsVectorLayer* remoteLayer )
{
  const QgsAttributeList& offlineAttrs = offlineLayer->attributeList();
  const QgsAttributeList& remoteAttrs = remoteLayer->attributeList();

  QMap < int /*offline attr*/, int /*remote attr*/ > attrLookup;
  // NOTE: use size of remoteAttrs, as offlineAttrs can have new attributes not yet synced
  for ( int i = 0; i < remoteAttrs.size(); i++ )
  {
    attrLookup.insert( offlineAttrs.at( i ), remoteAttrs.at( i ) );
  }

  return attrLookup;
}

void QgsOfflineEditing::showWarning( const QString& message )
{
  emit warning( tr( "Offline Editing Plugin" ), message );
}

sqlite3* QgsOfflineEditing::openLoggingDb()
{
  sqlite3* db = nullptr;
  QString dbPath = QgsProject::instance()->readEntry( PROJECT_ENTRY_SCOPE_OFFLINE, PROJECT_ENTRY_KEY_OFFLINE_DB_PATH );
  if ( !dbPath.isEmpty() )
  {
    QString absoluteDbPath = QgsProject::instance()->readPath( dbPath );
    int rc = sqlite3_open( absoluteDbPath.toUtf8().constData(), &db );
    if ( rc != SQLITE_OK )
    {
      QgsDebugMsg( "Could not open the spatialite logging database" );
      showWarning( tr( "Could not open the spatialite logging database" ) );
      sqlite3_close( db );
      db = nullptr;
    }
  }
  else
  {
    QgsDebugMsg( "dbPath is empty!" );
  }
  return db;
}

int QgsOfflineEditing::getOrCreateLayerId( sqlite3* db, const QString& qgisLayerId )
{
  QString sql = QString( "SELECT \"id\" FROM 'log_layer_ids' WHERE \"qgis_id\" = '%1'" ).arg( qgisLayerId );
  int layerId = sqlQueryInt( db, sql, -1 );
  if ( layerId == -1 )
  {
    // next layer id
    sql = "SELECT \"last_index\" FROM 'log_indices' WHERE \"name\" = 'layer_id'";
    int newLayerId = sqlQueryInt( db, sql, -1 );

    // insert layer
    sql = QString( "INSERT INTO 'log_layer_ids' VALUES (%1, '%2')" ).arg( newLayerId ).arg( qgisLayerId );
    sqlExec( db, sql );

    // increase layer_id
    // TODO: use trigger for auto increment?
    sql = QString( "UPDATE 'log_indices' SET 'last_index' = %1 WHERE \"name\" = 'layer_id'" ).arg( newLayerId + 1 );
    sqlExec( db, sql );

    layerId = newLayerId;
  }

  return layerId;
}

int QgsOfflineEditing::getCommitNo( sqlite3* db )
{
  QString sql = "SELECT \"last_index\" FROM 'log_indices' WHERE \"name\" = 'commit_no'";
  return sqlQueryInt( db, sql, -1 );
}

void QgsOfflineEditing::increaseCommitNo( sqlite3* db )
{
  QString sql = QString( "UPDATE 'log_indices' SET 'last_index' = %1 WHERE \"name\" = 'commit_no'" ).arg( getCommitNo( db ) + 1 );
  sqlExec( db, sql );
}

void QgsOfflineEditing::addFidLookup( sqlite3* db, int layerId, QgsFeatureId offlineFid, QgsFeatureId remoteFid )
{
  QString sql = QString( "INSERT INTO 'log_fids' VALUES ( %1, %2, %3 )" ).arg( layerId ).arg( offlineFid ).arg( remoteFid );
  sqlExec( db, sql );
}

QgsFeatureId QgsOfflineEditing::remoteFid( sqlite3* db, int layerId, QgsFeatureId offlineFid )
{
  QString sql = QString( "SELECT \"remote_fid\" FROM 'log_fids' WHERE \"layer_id\" = %1 AND \"offline_fid\" = %2" ).arg( layerId ).arg( offlineFid );
  return sqlQueryInt( db, sql, -1 );
}

QgsFeatureId QgsOfflineEditing::offlineFid( sqlite3* db, int layerId, QgsFeatureId remoteFid )
{
  QString sql = QString( "SELECT \"offline_fid\" FROM 'log_fids' WHERE \"layer_id\" = %1 AND \"remote_fid\" = %2" ).arg( layerId ).arg( remoteFid );
  return sqlQueryInt( db, sql, -1 );
}

bool QgsOfflineEditing::isAddedFeature( sqlite3* db, int layerId, QgsFeatureId fid )
{
  QString sql = QString( "SELECT COUNT(\"fid\") FROM 'log_added_features' WHERE \"layer_id\" = %1 AND \"fid\" = %2" ).arg( layerId ).arg( fid );
  return ( sqlQueryInt( db, sql, 0 ) > 0 );
}

int QgsOfflineEditing::sqlExec( sqlite3* db, const QString& sql )
{
  char * errmsg;
  int rc = sqlite3_exec( db, sql.toUtf8(), nullptr, nullptr, &errmsg );
  if ( rc != SQLITE_OK )
  {
    showWarning( errmsg );
  }
  return rc;
}

int QgsOfflineEditing::sqlQueryInt( sqlite3* db, const QString& sql, int defaultValue )
{
  sqlite3_stmt* stmt = nullptr;
  if ( sqlite3_prepare_v2( db, sql.toUtf8().constData(), -1, &stmt, nullptr ) != SQLITE_OK )
  {
    showWarning( sqlite3_errmsg( db ) );
    return defaultValue;
  }

  int value = defaultValue;
  int ret = sqlite3_step( stmt );
  if ( ret == SQLITE_ROW )
  {
    value = sqlite3_column_int( stmt, 0 );
  }
  sqlite3_finalize( stmt );

  return value;
}

QList<int> QgsOfflineEditing::sqlQueryInts( sqlite3* db, const QString& sql )
{
  QList<int> values;

  sqlite3_stmt* stmt = nullptr;
  if ( sqlite3_prepare_v2( db, sql.toUtf8().constData(), -1, &stmt, nullptr ) != SQLITE_OK )
  {
    showWarning( sqlite3_errmsg( db ) );
    return values;
  }

  int ret = sqlite3_step( stmt );
  while ( ret == SQLITE_ROW )
  {
    values << sqlite3_column_int( stmt, 0 );

    ret = sqlite3_step( stmt );
  }
  sqlite3_finalize( stmt );

  return values;
}

QList<QgsField> QgsOfflineEditing::sqlQueryAttributesAdded( sqlite3* db, const QString& sql )
{
  QList<QgsField> values;

  sqlite3_stmt* stmt = nullptr;
  if ( sqlite3_prepare_v2( db, sql.toUtf8().constData(), -1, &stmt, nullptr ) != SQLITE_OK )
  {
    showWarning( sqlite3_errmsg( db ) );
    return values;
  }

  int ret = sqlite3_step( stmt );
  while ( ret == SQLITE_ROW )
  {
    QgsField field( QString( reinterpret_cast< const char* >( sqlite3_column_text( stmt, 0 ) ) ),
                    static_cast< QVariant::Type >( sqlite3_column_int( stmt, 1 ) ),
                    "", // typeName
                    sqlite3_column_int( stmt, 2 ),
                    sqlite3_column_int( stmt, 3 ),
                    QString( reinterpret_cast< const char* >( sqlite3_column_text( stmt, 4 ) ) ) );
    values << field;

    ret = sqlite3_step( stmt );
  }
  sqlite3_finalize( stmt );

  return values;
}

QgsFeatureIds QgsOfflineEditing::sqlQueryFeaturesRemoved( sqlite3* db, const QString& sql )
{
  QgsFeatureIds values;

  sqlite3_stmt* stmt = nullptr;
  if ( sqlite3_prepare_v2( db, sql.toUtf8().constData(), -1, &stmt, nullptr ) != SQLITE_OK )
  {
    showWarning( sqlite3_errmsg( db ) );
    return values;
  }

  int ret = sqlite3_step( stmt );
  while ( ret == SQLITE_ROW )
  {
    values << sqlite3_column_int( stmt, 0 );

    ret = sqlite3_step( stmt );
  }
  sqlite3_finalize( stmt );

  return values;
}

QgsOfflineEditing::AttributeValueChanges QgsOfflineEditing::sqlQueryAttributeValueChanges( sqlite3* db, const QString& sql )
{
  AttributeValueChanges values;

  sqlite3_stmt* stmt = nullptr;
  if ( sqlite3_prepare_v2( db, sql.toUtf8().constData(), -1, &stmt, nullptr ) != SQLITE_OK )
  {
    showWarning( sqlite3_errmsg( db ) );
    return values;
  }

  int ret = sqlite3_step( stmt );
  while ( ret == SQLITE_ROW )
  {
    AttributeValueChange change;
    change.fid = sqlite3_column_int( stmt, 0 );
    change.attr = sqlite3_column_int( stmt, 1 );
    change.value = QString( reinterpret_cast< const char* >( sqlite3_column_text( stmt, 2 ) ) );
    values << change;

    ret = sqlite3_step( stmt );
  }
  sqlite3_finalize( stmt );

  return values;
}

QgsOfflineEditing::GeometryChanges QgsOfflineEditing::sqlQueryGeometryChanges( sqlite3* db, const QString& sql )
{
  GeometryChanges values;

  sqlite3_stmt* stmt = nullptr;
  if ( sqlite3_prepare_v2( db, sql.toUtf8().constData(), -1, &stmt, nullptr ) != SQLITE_OK )
  {
    showWarning( sqlite3_errmsg( db ) );
    return values;
  }

  int ret = sqlite3_step( stmt );
  while ( ret == SQLITE_ROW )
  {
    GeometryChange change;
    change.fid = sqlite3_column_int( stmt, 0 );
    change.geom_wkt = QString( reinterpret_cast< const char* >( sqlite3_column_text( stmt, 1 ) ) );
    values << change;

    ret = sqlite3_step( stmt );
  }
  sqlite3_finalize( stmt );

  return values;
}

void QgsOfflineEditing::committedAttributesAdded( const QString& qgisLayerId, const QList<QgsField>& addedAttributes )
{
  sqlite3* db = openLoggingDb();
  if ( !db )
    return;

  // insert log
  int layerId = getOrCreateLayerId( db, qgisLayerId );
  int commitNo = getCommitNo( db );

  for ( QList<QgsField>::const_iterator it = addedAttributes.begin(); it != addedAttributes.end(); ++it )
  {
    QgsField field = *it;
    QString sql = QString( "INSERT INTO 'log_added_attrs' VALUES ( %1, %2, '%3', %4, %5, %6, '%7' )" )
                  .arg( layerId )
                  .arg( commitNo )
                  .arg( field.name() )
                  .arg( field.type() )
                  .arg( field.length() )
                  .arg( field.precision() )
                  .arg( field.comment() );
    sqlExec( db, sql );
  }

  increaseCommitNo( db );
  sqlite3_close( db );
}

void QgsOfflineEditing::committedFeaturesAdded( const QString& qgisLayerId, const QgsFeatureList& addedFeatures )
{
  sqlite3* db = openLoggingDb();
  if ( !db )
    return;

  // insert log
  int layerId = getOrCreateLayerId( db, qgisLayerId );

  // get new feature ids from db
  QgsMapLayer* layer = QgsMapLayerRegistry::instance()->mapLayer( qgisLayerId );
  QgsDataSourceURI uri = QgsDataSourceURI( layer->source() );

  // only store feature ids
  QString sql = QString( "SELECT ROWID FROM '%1' ORDER BY ROWID DESC LIMIT %2" ).arg( uri.table() ).arg( addedFeatures.size() );
  QList<int> newFeatureIds = sqlQueryInts( db, sql );
  for ( int i = newFeatureIds.size() - 1; i >= 0; i-- )
  {
    QString sql = QString( "INSERT INTO 'log_added_features' VALUES ( %1, %2 )" )
                  .arg( layerId )
                  .arg( newFeatureIds.at( i ) );
    sqlExec( db, sql );
  }

  sqlite3_close( db );
}

void QgsOfflineEditing::committedFeaturesRemoved( const QString& qgisLayerId, const QgsFeatureIds& deletedFeatureIds )
{
  sqlite3* db = openLoggingDb();
  if ( !db )
    return;

  // insert log
  int layerId = getOrCreateLayerId( db, qgisLayerId );

  for ( QgsFeatureIds::const_iterator it = deletedFeatureIds.begin(); it != deletedFeatureIds.end(); ++it )
  {
    if ( isAddedFeature( db, layerId, *it ) )
    {
      // remove from added features log
      QString sql = QString( "DELETE FROM 'log_added_features' WHERE \"layer_id\" = %1 AND \"fid\" = %2" ).arg( layerId ).arg( *it );
      sqlExec( db, sql );
    }
    else
    {
      QString sql = QString( "INSERT INTO 'log_removed_features' VALUES ( %1, %2)" )
                    .arg( layerId )
                    .arg( *it );
      sqlExec( db, sql );
    }
  }

  sqlite3_close( db );
}

void QgsOfflineEditing::committedAttributeValuesChanges( const QString& qgisLayerId, const QgsChangedAttributesMap& changedAttrsMap )
{
  sqlite3* db = openLoggingDb();
  if ( !db )
    return;

  // insert log
  int layerId = getOrCreateLayerId( db, qgisLayerId );
  int commitNo = getCommitNo( db );

  for ( QgsChangedAttributesMap::const_iterator cit = changedAttrsMap.begin(); cit != changedAttrsMap.end(); ++cit )
  {
    QgsFeatureId fid = cit.key();
    if ( isAddedFeature( db, layerId, fid ) )
    {
      // skip added features
      continue;
    }
    QgsAttributeMap attrMap = cit.value();
    for ( QgsAttributeMap::const_iterator it = attrMap.begin(); it != attrMap.end(); ++it )
    {
      QString sql = QString( "INSERT INTO 'log_feature_updates' VALUES ( %1, %2, %3, %4, '%5' )" )
                    .arg( layerId )
                    .arg( commitNo )
                    .arg( fid )
                    .arg( it.key() ) // attr
                    .arg( it.value().toString() ); // value
      sqlExec( db, sql );
    }
  }

  increaseCommitNo( db );
  sqlite3_close( db );
}

void QgsOfflineEditing::committedGeometriesChanges( const QString& qgisLayerId, const QgsGeometryMap& changedGeometries )
{
  sqlite3* db = openLoggingDb();
  if ( !db )
    return;

  // insert log
  int layerId = getOrCreateLayerId( db, qgisLayerId );
  int commitNo = getCommitNo( db );

  for ( QgsGeometryMap::const_iterator it = changedGeometries.begin(); it != changedGeometries.end(); ++it )
  {
    QgsFeatureId fid = it.key();
    if ( isAddedFeature( db, layerId, fid ) )
    {
      // skip added features
      continue;
    }
    QgsGeometry geom = it.value();
    QString sql = QString( "INSERT INTO 'log_geometry_updates' VALUES ( %1, %2, %3, '%4' )" )
                  .arg( layerId )
                  .arg( commitNo )
                  .arg( fid )
                  .arg( geom.exportToWkt() );
    sqlExec( db, sql );

    // TODO: use WKB instead of WKT?
  }

  increaseCommitNo( db );
  sqlite3_close( db );
}

void QgsOfflineEditing::startListenFeatureChanges()
{
  QgsVectorLayer* vLayer = qobject_cast<QgsVectorLayer *>( sender() );
  // enable logging, check if editBuffer is not null
  if ( vLayer->editBuffer() )
  {
    connect( vLayer->editBuffer(), SIGNAL( committedAttributesAdded( const QString&, const QList<QgsField>& ) ),
             this, SLOT( committedAttributesAdded( const QString&, const QList<QgsField>& ) ) );
    connect( vLayer->editBuffer(), SIGNAL( committedAttributeValuesChanges( const QString&, const QgsChangedAttributesMap& ) ),
             this, SLOT( committedAttributeValuesChanges( const QString&, const QgsChangedAttributesMap& ) ) );
    connect( vLayer->editBuffer(), SIGNAL( committedGeometriesChanges( const QString&, const QgsGeometryMap& ) ),
             this, SLOT( committedGeometriesChanges( const QString&, const QgsGeometryMap& ) ) );
  }
  connect( vLayer, SIGNAL( committedFeaturesAdded( const QString&, const QgsFeatureList& ) ),
           this, SLOT( committedFeaturesAdded( const QString&, const QgsFeatureList& ) ) );
  connect( vLayer, SIGNAL( committedFeaturesRemoved( const QString&, const QgsFeatureIds& ) ),
           this, SLOT( committedFeaturesRemoved( const QString&, const QgsFeatureIds& ) ) );
}

void QgsOfflineEditing::stopListenFeatureChanges()
{
  QgsVectorLayer* vLayer = qobject_cast<QgsVectorLayer *>( sender() );
  // disable logging, check if editBuffer is not null
  if ( vLayer->editBuffer() )
  {
    disconnect( vLayer->editBuffer(), SIGNAL( committedAttributesAdded( const QString&, const QList<QgsField>& ) ),
                this, SLOT( committedAttributesAdded( const QString&, const QList<QgsField>& ) ) );
    disconnect( vLayer->editBuffer(), SIGNAL( committedAttributeValuesChanges( const QString&, const QgsChangedAttributesMap& ) ),
                this, SLOT( committedAttributeValuesChanges( const QString&, const QgsChangedAttributesMap& ) ) );
    disconnect( vLayer->editBuffer(), SIGNAL( committedGeometriesChanges( const QString&, const QgsGeometryMap& ) ),
                this, SLOT( committedGeometriesChanges( const QString&, const QgsGeometryMap& ) ) );
  }
  disconnect( vLayer, SIGNAL( committedFeaturesAdded( const QString&, const QgsFeatureList& ) ),
              this, SLOT( committedFeaturesAdded( const QString&, const QgsFeatureList& ) ) );
  disconnect( vLayer, SIGNAL( committedFeaturesRemoved( const QString&, const QgsFeatureIds& ) ),
              this, SLOT( committedFeaturesRemoved( const QString&, const QgsFeatureIds& ) ) );
}

void QgsOfflineEditing::layerAdded( QgsMapLayer* layer )
{
  // detect offline layer
  if ( layer->customProperty( CUSTOM_PROPERTY_IS_OFFLINE_EDITABLE, false ).toBool() )
  {
    QgsVectorLayer* vLayer = qobject_cast<QgsVectorLayer *>( layer );
    connect( vLayer, SIGNAL( editingStarted() ), this, SLOT( startListenFeatureChanges() ) );
    connect( vLayer, SIGNAL( editingStopped() ), this, SLOT( stopListenFeatureChanges() ) );
  }
}


