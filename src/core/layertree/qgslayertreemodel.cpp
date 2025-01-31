/***************************************************************************
  qgslayertreemodel.cpp
  --------------------------------------
  Date                 : May 2014
  Copyright            : (C) 2014 by Martin Dobias
  Email                : wonder dot sk at gmail dot com
 ***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include "qgslayertreemodel.h"

#include "qgslayertree.h"
#include "qgslayertreeutils.h"
#include "qgslayertreemodellegendnode.h"

#include <QMimeData>
#include <QTextStream>

#include "qgsdataitem.h"
#include "qgsmaphittest.h"
#include "qgsmaplayerlegend.h"
#include "qgsmaplayerstylemanager.h"
#include "qgspluginlayer.h"
#include "qgsrasterlayer.h"
#include "qgsrendererv2.h"
#include "qgssymbollayerv2utils.h"
#include "qgsvectorlayer.h"

///@cond PRIVATE

/** In order to support embedded widgets in layer tree view, the model
 * generates one placeholder legend node for each embedded widget.
 * The placeholder will be replaced by an embedded widget in QgsLayerTreeView
 */
class EmbeddedWidgetLegendNode : public QgsLayerTreeModelLegendNode
{
  public:
    EmbeddedWidgetLegendNode( QgsLayerTreeLayer* nodeL )
        : QgsLayerTreeModelLegendNode( nodeL )
    {
      // we need a valid rule key to allow the model to build a tree out of legend nodes
      // if that's possible (if there is a node without a rule key, building of tree is cancelled)
      mRuleKey = "embedded-widget-" + QUuid::createUuid().toString();
    }

    virtual QVariant data( int role ) const override
    {
      if ( role == RuleKeyRole )
        return mRuleKey;
      return QVariant();
    }

  private:
    QString mRuleKey;
};

///@endcond

QgsLayerTreeModel::QgsLayerTreeModel( QgsLayerTreeGroup* rootNode, QObject *parent )
    : QAbstractItemModel( parent )
    , mRootNode( rootNode )
    , mFlags( ShowLegend | AllowLegendChangeState | DeferredLegendInvalidation )
    , mAutoCollapseLegendNodesCount( -1 )
    , mLegendFilterByScale( 0 )
    , mLegendFilterUsesExtent( false )
    , mLegendMapViewMupp( 0 )
    , mLegendMapViewDpi( 0 )
    , mLegendMapViewScale( 0 )
{
  connectToRootNode();

  mFontLayer.setBold( true );

  connect( &mDeferLegendInvalidationTimer, SIGNAL( timeout() ), this, SLOT( invalidateLegendMapBasedData() ) );
  mDeferLegendInvalidationTimer.setSingleShot( true );
}

QgsLayerTreeModel::~QgsLayerTreeModel()
{
  legendCleanup();
}

QgsLayerTreeNode* QgsLayerTreeModel::index2node( const QModelIndex& index ) const
{
  if ( !index.isValid() )
    return mRootNode;

  QObject* obj = reinterpret_cast<QObject*>( index.internalPointer() );
  return qobject_cast<QgsLayerTreeNode*>( obj );
}


int QgsLayerTreeModel::rowCount( const QModelIndex &parent ) const
{
  if ( QgsLayerTreeModelLegendNode* nodeLegend = index2legendNode( parent ) )
    return legendNodeRowCount( nodeLegend );

  QgsLayerTreeNode* n = index2node( parent );
  if ( !n )
    return 0;

  if ( QgsLayerTree::isLayer( n ) )
  {
    if ( !testFlag( ShowLegend ) )
      return 0;

    return legendRootRowCount( QgsLayerTree::toLayer( n ) );
  }

  return n->children().count();
}

int QgsLayerTreeModel::columnCount( const QModelIndex &parent ) const
{
  Q_UNUSED( parent );
  return 1;
}

QModelIndex QgsLayerTreeModel::index( int row, int column, const QModelIndex &parent ) const
{
  if ( column < 0 || column >= columnCount( parent ) ||
       row < 0 || row >= rowCount( parent ) )
    return QModelIndex();

  if ( QgsLayerTreeModelLegendNode* nodeLegend = index2legendNode( parent ) )
    return legendNodeIndex( row, column, nodeLegend );

  QgsLayerTreeNode *n = index2node( parent );
  if ( !n )
    return QModelIndex(); // have no children

  if ( testFlag( ShowLegend ) && QgsLayerTree::isLayer( n ) )
  {
    return legendRootIndex( row, column, QgsLayerTree::toLayer( n ) );
  }

  return createIndex( row, column, static_cast<QObject*>( n->children().at( row ) ) );
}


QModelIndex QgsLayerTreeModel::parent( const QModelIndex &child ) const
{
  if ( !child.isValid() )
    return QModelIndex();

  if ( QgsLayerTreeNode *n = index2node( child ) )
  {
    return indexOfParentLayerTreeNode( n->parent() ); // must not be null
  }
  else if ( QgsLayerTreeModelLegendNode* legendNode = index2legendNode( child ) )
  {
    return legendParent( legendNode );
  }
  else
  {
    Q_ASSERT( false ); // no other node types!
    return QModelIndex();
  }

}


QModelIndex QgsLayerTreeModel::indexOfParentLayerTreeNode( QgsLayerTreeNode* parentNode ) const
{
  Q_ASSERT( parentNode );

  QgsLayerTreeNode* grandParentNode = parentNode->parent();
  if ( !grandParentNode )
    return QModelIndex();  // root node -> invalid index

  int row = grandParentNode->children().indexOf( parentNode );
  Q_ASSERT( row >= 0 );

  return createIndex( row, 0, static_cast<QObject*>( parentNode ) );
}


QVariant QgsLayerTreeModel::data( const QModelIndex &index, int role ) const
{
  if ( !index.isValid() || index.column() > 1 )
    return QVariant();

  if ( QgsLayerTreeModelLegendNode* sym = index2legendNode( index ) )
    return legendNodeData( sym, role );

  QgsLayerTreeNode* node = index2node( index );
  if ( role == Qt::DisplayRole || role == Qt::EditRole )
  {
    if ( QgsLayerTree::isGroup( node ) )
      return QgsLayerTree::toGroup( node )->name();

    if ( QgsLayerTree::isLayer( node ) )
    {
      QgsLayerTreeLayer* nodeLayer = QgsLayerTree::toLayer( node );
      QString name = nodeLayer->layerName();
      if ( nodeLayer->customProperty( "showFeatureCount", 0 ).toInt() && role == Qt::DisplayRole )
      {
        QgsVectorLayer* vlayer = qobject_cast<QgsVectorLayer*>( nodeLayer->layer() );
        if ( vlayer && vlayer->featureCount() >= 0 )
          name += QString( " [%1]" ).arg( vlayer->featureCount() );
      }
      return name;
    }
  }
  else if ( role == Qt::DecorationRole && index.column() == 0 )
  {
    if ( QgsLayerTree::isGroup( node ) )
      return iconGroup();

    if ( QgsLayerTree::isLayer( node ) )
    {
      QgsLayerTreeLayer *nodeLayer = QgsLayerTree::toLayer( node );

      QgsMapLayer *layer = nodeLayer->layer();
      if ( !layer )
        return QVariant();

      // icons possibly overriding default icon
      if ( layer->type() == QgsMapLayer::RasterLayer )
      {
        if ( testFlag( ShowRasterPreviewIcon ) )
        {
          QgsRasterLayer* rlayer = qobject_cast<QgsRasterLayer *>( layer );
          // avoid to create preview for wms that imply a better multi thread management
          // due to async nature of wms.
          // The following two line of code is part of the fix of
          // https://issues.qgis.org/issues/16803
          if ( rlayer->providerType() == QLatin1String( "wms" ) )
            return QgsLayerItem::iconRaster();
          return QIcon( QPixmap::fromImage( rlayer->previewAsImage( QSize( 32, 32 ) ) ) );
        }
        else
        {
          return QgsLayerItem::iconRaster();
        }
      }

      QgsVectorLayer *vlayer = dynamic_cast<QgsVectorLayer*>( layer );
      QIcon icon;

      // if there's just on legend entry that should be embedded in layer - do that!
      if ( testFlag( ShowLegend ) && legendEmbeddedInParent( nodeLayer ) )
      {
        icon = legendIconEmbeddedInParent( nodeLayer );
      }
      else if ( vlayer && layer->type() == QgsMapLayer::VectorLayer )
      {
        if ( vlayer->geometryType() == QGis::Point )
          icon = QgsLayerItem::iconPoint();
        else if ( vlayer->geometryType() == QGis::Line )
          icon = QgsLayerItem::iconLine();
        else if ( vlayer->geometryType() == QGis::Polygon )
          icon = QgsLayerItem::iconPolygon();
        else if ( vlayer->geometryType() == QGis::NoGeometry )
          icon = QgsLayerItem::iconTable();
        else
          icon = QgsLayerItem::iconDefault();
      }

      if ( vlayer && vlayer->isEditable() )
      {
        QPixmap pixmap( icon.pixmap( 16, 16 ) );

        QPainter painter( &pixmap );
        painter.drawPixmap( 0, 0, 16, 16, QgsApplication::getThemePixmap( vlayer->isModified() ? "/mIconEditableEdits.png" : "/mIconEditable.png" ) );
        painter.end();

        icon = QIcon( pixmap );
      }

      return icon;
    }
  }
  else if ( role == Qt::CheckStateRole )
  {
    if ( !testFlag( AllowNodeChangeVisibility ) )
      return QVariant();

    if ( QgsLayerTree::isLayer( node ) )
    {
      QgsLayerTreeLayer* nodeLayer = QgsLayerTree::toLayer( node );
      if ( nodeLayer->layer() && nodeLayer->layer()->type() == QgsMapLayer::VectorLayer )
      {
        if ( qobject_cast<QgsVectorLayer*>( nodeLayer->layer() )->geometryType() == QGis::NoGeometry )
          return QVariant(); // do not show checkbox for non-spatial tables
      }
      return nodeLayer->isVisible();
    }
    else if ( QgsLayerTree::isGroup( node ) )
    {
      QgsLayerTreeGroup* nodeGroup = QgsLayerTree::toGroup( node );
      return nodeGroup->isVisible();
    }
  }
  else if ( role == Qt::FontRole )
  {
    QFont f( QgsLayerTree::isLayer( node ) ? mFontLayer : ( QgsLayerTree::isGroup( node ) ? mFontGroup : QFont() ) );
    if ( node->customProperty( "embedded" ).toInt() )
      f.setItalic( true );
    if ( index == mCurrentIndex )
      f.setUnderline( true );
    return f;
  }
  else if ( role == Qt::ForegroundRole )
  {
    QBrush brush( Qt::black, Qt::SolidPattern );
    if ( QgsLayerTree::isLayer( node ) )
    {
      const QgsMapLayer* layer = QgsLayerTree::toLayer( node )->layer();
      if ( layer && !layer->isInScaleRange( mLegendMapViewScale ) )
      {
        brush.setColor( Qt::lightGray );
      }
    }
    return brush;
  }
  else if ( role == Qt::ToolTipRole )
  {
    if ( QgsLayerTree::isLayer( node ) )
    {
      if ( QgsMapLayer *layer = QgsLayerTree::toLayer( node )->layer() )
      {
        QStringList parts;
        QString title = layer->title().isEmpty() ? layer->shortName() : layer->title();
        if ( title.isEmpty() )
          title = layer->name();
        title = "<b>" + title + "</b>";
        if ( layer->crs().isValid() )
          title = tr( "%1 (%2)" ).arg( title, layer->crs().authid() );

        parts << title;

        if ( !layer->abstract().isEmpty() )
          parts << "<br/>" + layer->abstract().replace( QLatin1String( "\n" ), QLatin1String( "<br/>" ) );
        parts << "<i>" + layer->publicSource() + "</i>";
        return parts.join( "<br/>" );
      }
    }
  }

  return QVariant();
}


Qt::ItemFlags QgsLayerTreeModel::flags( const QModelIndex& index ) const
{
  if ( !index.isValid() )
  {
    Qt::ItemFlags rootFlags = Qt::ItemFlags();
    if ( testFlag( AllowNodeReorder ) )
      rootFlags |= Qt::ItemIsDropEnabled;
    return rootFlags;
  }

  if ( QgsLayerTreeModelLegendNode* symn = index2legendNode( index ) )
    return legendNodeFlags( symn );

  Qt::ItemFlags f = Qt::ItemIsEnabled | Qt::ItemIsSelectable;

  if ( testFlag( AllowNodeRename ) )
    f |= Qt::ItemIsEditable;

  QgsLayerTreeNode* node = index2node( index );
  bool isEmbedded = node->customProperty( "embedded" ).toInt();

  if ( testFlag( AllowNodeReorder ) )
  {
    // only root embedded nodes can be reordered
    if ( !isEmbedded || ( isEmbedded && node->parent() && !node->parent()->customProperty( "embedded" ).toInt() ) )
      f |= Qt::ItemIsDragEnabled;
  }

  if ( testFlag( AllowNodeChangeVisibility ) && ( QgsLayerTree::isLayer( node ) || QgsLayerTree::isGroup( node ) ) )
    f |= Qt::ItemIsUserCheckable;

  if ( testFlag( AllowNodeReorder ) && QgsLayerTree::isGroup( node ) && !isEmbedded )
    f |= Qt::ItemIsDropEnabled;

  return f;
}

bool QgsLayerTreeModel::setData( const QModelIndex& index, const QVariant& value, int role )
{
  QgsLayerTreeModelLegendNode *sym = index2legendNode( index );
  if ( sym )
  {
    if ( role == Qt::CheckStateRole && !testFlag( AllowLegendChangeState ) )
      return false;
    bool res = sym->setData( value, role );
    if ( res )
      emit dataChanged( index, index );
    return res;
  }

  QgsLayerTreeNode* node = index2node( index );
  if ( !node )
    return QgsLayerTreeModel::setData( index, value, role );

  if ( role == Qt::CheckStateRole )
  {
    if ( !testFlag( AllowNodeChangeVisibility ) )
      return false;

    if ( QgsLayerTree::isLayer( node ) )
    {
      QgsLayerTreeLayer* layer = QgsLayerTree::toLayer( node );
      layer->setVisible( static_cast< Qt::CheckState >( value.toInt() ) );
      return true;
    }

    if ( QgsLayerTree::isGroup( node ) )
    {
      QgsLayerTreeGroup* group = QgsLayerTree::toGroup( node );
      group->setVisible( static_cast< Qt::CheckState >( value.toInt() ) );
      return true;
    }

    return true;
  }
  else if ( role == Qt::EditRole )
  {
    if ( !testFlag( AllowNodeRename ) )
      return false;

    if ( QgsLayerTree::isLayer( node ) )
    {
      QgsLayerTreeLayer* layer = QgsLayerTree::toLayer( node );
      layer->setLayerName( value.toString() );
      emit dataChanged( index, index );
    }
    else if ( QgsLayerTree::isGroup( node ) )
    {
      QgsLayerTree::toGroup( node )->setName( value.toString() );
      emit dataChanged( index, index );
    }
  }

  return QAbstractItemModel::setData( index, value, role );
}

QModelIndex QgsLayerTreeModel::node2index( QgsLayerTreeNode* node ) const
{
  if ( !node->parent() )
    return QModelIndex(); // this is the only root item -> invalid index

  QModelIndex parentIndex = node2index( node->parent() );

  int row = node->parent()->children().indexOf( node );
  Q_ASSERT( row >= 0 );
  return index( row, 0, parentIndex );
}


static bool _isChildOfNode( QgsLayerTreeNode* child, QgsLayerTreeNode* node )
{
  if ( !child->parent() )
    return false;

  if ( child->parent() == node )
    return true;

  return _isChildOfNode( child->parent(), node );
}

static bool _isChildOfNodes( QgsLayerTreeNode* child, const QList<QgsLayerTreeNode*>& nodes )
{
  Q_FOREACH ( QgsLayerTreeNode* n, nodes )
  {
    if ( _isChildOfNode( child, n ) )
      return true;
  }

  return false;
}


QList<QgsLayerTreeNode*> QgsLayerTreeModel::indexes2nodes( const QModelIndexList& list, bool skipInternal ) const
{
  QList<QgsLayerTreeNode*> nodes;
  Q_FOREACH ( const QModelIndex& index, list )
  {
    QgsLayerTreeNode* node = index2node( index );
    if ( !node )
      continue;

    nodes << node;
  }

  if ( !skipInternal )
    return nodes;

  // remove any children of nodes if both parent node and children are selected
  QList<QgsLayerTreeNode*> nodesFinal;
  Q_FOREACH ( QgsLayerTreeNode* node, nodes )
  {
    if ( !_isChildOfNodes( node, nodes ) )
      nodesFinal << node;
  }

  return nodesFinal;
}

bool QgsLayerTreeModel::isIndexSymbologyNode( const QModelIndex& index ) const
{
  return nullptr != index2legendNode( index );
}

QgsLayerTreeLayer* QgsLayerTreeModel::layerNodeForSymbologyNode( const QModelIndex& index ) const
{
  QgsLayerTreeModelLegendNode* symNode = index2legendNode( index );
  return symNode ? symNode->layerNode() : nullptr;
}

QgsLayerTreeGroup*QgsLayerTreeModel::rootGroup() const
{
  return mRootNode;
}

void QgsLayerTreeModel::setRootGroup( QgsLayerTreeGroup* newRootGroup )
{
  beginResetModel();

  disconnectFromRootNode();

  Q_ASSERT( mLegend.isEmpty() );

  mRootNode = newRootGroup;

  endResetModel();

  connectToRootNode();
}

void QgsLayerTreeModel::refreshLayerLegend( QgsLayerTreeLayer* nodeLayer )
{
  // update title
  QModelIndex idx = node2index( nodeLayer );
  emit dataChanged( idx, idx );

  // update children
  int oldNodeCount = rowCount( idx );
  beginRemoveRows( idx, 0, oldNodeCount - 1 );
  removeLegendFromLayer( nodeLayer );
  endRemoveRows();

  addLegendToLayer( nodeLayer );
  int newNodeCount = rowCount( idx );

  // automatic collapse of legend nodes - useful if a layer has many legend nodes
  if ( mAutoCollapseLegendNodesCount != -1 && oldNodeCount != newNodeCount && newNodeCount >= mAutoCollapseLegendNodesCount )
    nodeLayer->setExpanded( false );
}

QModelIndex QgsLayerTreeModel::currentIndex() const
{
  return mCurrentIndex;
}

void QgsLayerTreeModel::setCurrentIndex( const QModelIndex& currentIndex )
{
  QModelIndex oldIndex = mCurrentIndex;
  mCurrentIndex = currentIndex;

  if ( oldIndex.isValid() )
    emit dataChanged( oldIndex, oldIndex );
  if ( currentIndex.isValid() )
    emit dataChanged( currentIndex, currentIndex );
}


void QgsLayerTreeModel::setLayerTreeNodeFont( int nodeType, const QFont& font )
{
  if ( nodeType == QgsLayerTreeNode::NodeGroup )
  {
    if ( mFontGroup != font )
    {
      mFontGroup = font;
      recursivelyEmitDataChanged();
    }
  }
  else if ( nodeType == QgsLayerTreeNode::NodeLayer )
  {
    if ( mFontLayer != font )
    {
      mFontLayer = font;
      recursivelyEmitDataChanged();
    }
  }
  else
  {
    QgsDebugMsg( "invalid node type" );
  }
}


QFont QgsLayerTreeModel::layerTreeNodeFont( int nodeType ) const
{
  if ( nodeType == QgsLayerTreeNode::NodeGroup )
    return mFontGroup;
  else if ( nodeType == QgsLayerTreeNode::NodeLayer )
    return mFontLayer;
  else
  {
    QgsDebugMsg( "invalid node type" );
    return QFont();
  }
}

void QgsLayerTreeModel::setLegendFilterByScale( double scaleDenominator )
{
  mLegendFilterByScale = scaleDenominator;

  // this could be later done in more efficient way
  // by just updating active legend nodes, without refreshing original legend nodes
  Q_FOREACH ( QgsLayerTreeLayer* nodeLayer, mRootNode->findLayers() )
    refreshLayerLegend( nodeLayer );
}

void QgsLayerTreeModel::setLegendFilterByMap( const QgsMapSettings* settings )
{
  setLegendFilter( settings, /* useExtent = */ true );
}

void QgsLayerTreeModel::setLegendFilter( const QgsMapSettings* settings, bool useExtent, const QgsGeometry& polygon, bool useExpressions )
{
  if ( settings && settings->hasValidSettings() )
  {
    mLegendFilterMapSettings.reset( new QgsMapSettings( *settings ) );
    mLegendFilterMapSettings->setLayerStyleOverrides( mLayerStyleOverrides );
    QgsMapHitTest::LayerFilterExpression exprs;
    mLegendFilterUsesExtent = useExtent;
    // collect expression filters
    if ( useExpressions )
    {
      Q_FOREACH ( QgsLayerTreeLayer* nodeLayer, mRootNode->findLayers() )
      {
        bool enabled;
        QString expr = QgsLayerTreeUtils::legendFilterByExpression( *nodeLayer, &enabled );
        if ( enabled && !expr.isEmpty() )
        {
          exprs[ nodeLayer->layerId()] = expr;
        }
      }
    }
    bool polygonValid = !polygon.isEmpty() && polygon.type() == QGis::Polygon;
    if ( useExpressions && !useExtent && !polygonValid ) // only expressions
    {
      mLegendFilterHitTest.reset( new QgsMapHitTest( *mLegendFilterMapSettings, exprs ) );
    }
    else
    {
      mLegendFilterHitTest.reset( new QgsMapHitTest( *mLegendFilterMapSettings, polygon, exprs ) );
    }
    mLegendFilterHitTest->run();
  }
  else
  {
    if ( !mLegendFilterMapSettings )
      return; // no change

    mLegendFilterMapSettings.reset();
    mLegendFilterHitTest.reset();
  }

  // temporarily disable autocollapse so that legend nodes stay visible
  int bkAutoCollapse = autoCollapseLegendNodes();
  setAutoCollapseLegendNodes( -1 );

  // this could be later done in more efficient way
  // by just updating active legend nodes, without refreshing original legend nodes
  Q_FOREACH ( QgsLayerTreeLayer* nodeLayer, mRootNode->findLayers() )
    refreshLayerLegend( nodeLayer );

  setAutoCollapseLegendNodes( bkAutoCollapse );
}

void QgsLayerTreeModel::setLegendMapViewData( double mapUnitsPerPixel, int dpi, double scale )
{
  if ( mLegendMapViewDpi == dpi && qgsDoubleNear( mLegendMapViewMupp, mapUnitsPerPixel ) && qgsDoubleNear( mLegendMapViewScale, scale ) )
    return;

  mLegendMapViewMupp = mapUnitsPerPixel;
  mLegendMapViewDpi = dpi;
  mLegendMapViewScale = scale;

  // now invalidate legend nodes!
  legendInvalidateMapBasedData();

  refreshScaleBasedLayers();
}

void QgsLayerTreeModel::legendMapViewData( double* mapUnitsPerPixel, int* dpi, double* scale )
{
  if ( mapUnitsPerPixel ) *mapUnitsPerPixel = mLegendMapViewMupp;
  if ( dpi ) *dpi = mLegendMapViewDpi;
  if ( scale ) *scale = mLegendMapViewScale;
}

QMap<QString, QString> QgsLayerTreeModel::layerStyleOverrides() const
{
  return mLayerStyleOverrides;
}

void QgsLayerTreeModel::setLayerStyleOverrides( const QMap<QString, QString>& overrides )
{
  mLayerStyleOverrides = overrides;
}

void QgsLayerTreeModel::nodeWillAddChildren( QgsLayerTreeNode* node, int indexFrom, int indexTo )
{
  Q_ASSERT( node );
  beginInsertRows( node2index( node ), indexFrom, indexTo );
}

static QList<QgsLayerTreeLayer*> _layerNodesInSubtree( QgsLayerTreeNode* node, int indexFrom, int indexTo )
{
  QList<QgsLayerTreeNode*> children = node->children();
  QList<QgsLayerTreeLayer*> newLayerNodes;
  for ( int i = indexFrom; i <= indexTo; ++i )
  {
    QgsLayerTreeNode* child = children.at( i );
    if ( QgsLayerTree::isLayer( child ) )
      newLayerNodes << QgsLayerTree::toLayer( child );
    else if ( QgsLayerTree::isGroup( child ) )
      newLayerNodes << QgsLayerTree::toGroup( child )->findLayers();
  }
  return newLayerNodes;
}

void QgsLayerTreeModel::nodeAddedChildren( QgsLayerTreeNode* node, int indexFrom, int indexTo )
{
  Q_ASSERT( node );

  endInsertRows();

  Q_FOREACH ( QgsLayerTreeLayer* newLayerNode, _layerNodesInSubtree( node, indexFrom, indexTo ) )
    connectToLayer( newLayerNode );
}

void QgsLayerTreeModel::nodeWillRemoveChildren( QgsLayerTreeNode* node, int indexFrom, int indexTo )
{
  Q_ASSERT( node );

  beginRemoveRows( node2index( node ), indexFrom, indexTo );

  // disconnect from layers and remove their legend
  Q_FOREACH ( QgsLayerTreeLayer* nodeLayer, _layerNodesInSubtree( node, indexFrom, indexTo ) )
    disconnectFromLayer( nodeLayer );
}

void QgsLayerTreeModel::nodeRemovedChildren()
{
  endRemoveRows();
}

void QgsLayerTreeModel::nodeVisibilityChanged( QgsLayerTreeNode* node )
{
  Q_ASSERT( node );

  QModelIndex index = node2index( node );
  emit dataChanged( index, index );
}

void QgsLayerTreeModel::nodeNameChanged( QgsLayerTreeNode* node, const QString& name )
{
  Q_UNUSED( name );
  Q_ASSERT( node );

  QModelIndex index = node2index( node );
  emit dataChanged( index, index );
}


void QgsLayerTreeModel::nodeCustomPropertyChanged( QgsLayerTreeNode* node, const QString& key )
{
  if ( QgsLayerTree::isLayer( node ) && key == "showFeatureCount" )
    refreshLayerLegend( QgsLayerTree::toLayer( node ) );
}


void QgsLayerTreeModel::nodeLayerLoaded()
{
  QgsLayerTreeLayer* nodeLayer = qobject_cast<QgsLayerTreeLayer*>( sender() );
  if ( !nodeLayer )
    return;

  // deferred connection to the layer
  connectToLayer( nodeLayer );
}

void QgsLayerTreeModel::nodeLayerWillBeUnloaded()
{
  QgsLayerTreeLayer* nodeLayer = qobject_cast<QgsLayerTreeLayer*>( sender() );
  if ( !nodeLayer )
    return;

  disconnectFromLayer( nodeLayer );

  // wait for the layer to appear again
  connect( nodeLayer, SIGNAL( layerLoaded() ), this, SLOT( nodeLayerLoaded() ) );
}

void QgsLayerTreeModel::layerLegendChanged()
{
  if ( !testFlag( ShowLegend ) )
    return;

  QgsMapLayer* layer = qobject_cast<QgsMapLayer*>( sender() );
  if ( !layer )
    return;

  QgsLayerTreeLayer* nodeLayer = mRootNode->findLayer( layer->id() );
  if ( !nodeLayer )
    return;

  refreshLayerLegend( nodeLayer );
}

void QgsLayerTreeModel::layerNeedsUpdate()
{
  QgsMapLayer* layer = qobject_cast<QgsMapLayer*>( sender() );
  if ( !layer )
    return;

  QgsLayerTreeLayer* nodeLayer = mRootNode->findLayer( layer->id() );
  if ( !nodeLayer )
    return;

  QModelIndex index = node2index( nodeLayer );
  emit dataChanged( index, index );

  if ( nodeLayer->customProperty( "showFeatureCount" ).toInt() )
    refreshLayerLegend( nodeLayer );
}


void QgsLayerTreeModel::legendNodeDataChanged()
{
  QgsLayerTreeModelLegendNode* legendNode = qobject_cast<QgsLayerTreeModelLegendNode*>( sender() );
  if ( !legendNode )
    return;

  QModelIndex index = legendNode2index( legendNode );
  if ( index.isValid() )
    emit dataChanged( index, index );
}


void QgsLayerTreeModel::connectToLayer( QgsLayerTreeLayer* nodeLayer )
{
  if ( !nodeLayer->layer() )
  {
    // in order to connect to layer, we need to have it loaded.
    // keep an eye on the layer ID: once loaded, we will use it
    connect( nodeLayer, SIGNAL( layerLoaded() ), this, SLOT( nodeLayerLoaded() ) );
    return;
  }

  // watch if the layer is getting removed
  connect( nodeLayer, SIGNAL( layerWillBeUnloaded() ), this, SLOT( nodeLayerWillBeUnloaded() ) );

  if ( testFlag( ShowLegend ) )
  {
    addLegendToLayer( nodeLayer );

    // automatic collapse of legend nodes - useful if a layer has many legend nodes
    if ( !mRootNode->customProperty( "loading" ).toBool() )
    {
      if ( mAutoCollapseLegendNodesCount != -1 && rowCount( node2index( nodeLayer ) )  >= mAutoCollapseLegendNodesCount )
        nodeLayer->setExpanded( false );
    }
  }

  QgsMapLayer* layer = nodeLayer->layer();
  connect( layer, SIGNAL( legendChanged() ), this, SLOT( layerLegendChanged() ), Qt::UniqueConnection );

  if ( layer->type() == QgsMapLayer::VectorLayer )
  {
    // using unique connection because there may be temporarily more nodes for a layer than just one
    // which would create multiple connections, however disconnect() would disconnect all multiple connections
    // even if we wanted to disconnect just one connection in each call.
    connect( layer, SIGNAL( editingStarted() ), this, SLOT( layerNeedsUpdate() ), Qt::UniqueConnection );
    connect( layer, SIGNAL( editingStopped() ), this, SLOT( layerNeedsUpdate() ), Qt::UniqueConnection );
    connect( layer, SIGNAL( layerModified() ), this, SLOT( layerNeedsUpdate() ), Qt::UniqueConnection );
  }
}

// try to find out if the layer ID is present in the tree multiple times
static int _numLayerCount( QgsLayerTreeGroup* group, const QString& layerId )
{
  int count = 0;
  Q_FOREACH ( QgsLayerTreeNode* child, group->children() )
  {
    if ( QgsLayerTree::isLayer( child ) )
    {
      if ( QgsLayerTree::toLayer( child )->layerId() == layerId )
        count++;
    }
    else if ( QgsLayerTree::isGroup( child ) )
    {
      count += _numLayerCount( QgsLayerTree::toGroup( child ), layerId );
    }
  }
  return count;
}

void QgsLayerTreeModel::disconnectFromLayer( QgsLayerTreeLayer* nodeLayer )
{
  disconnect( nodeLayer, nullptr, this, nullptr ); // disconnect from delayed load of layer

  if ( !nodeLayer->layer() )
    return; // we were never connected

  if ( testFlag( ShowLegend ) )
  {
    removeLegendFromLayer( nodeLayer );
  }

  if ( _numLayerCount( mRootNode, nodeLayer->layerId() ) == 1 )
  {
    // last instance of the layer in the tree: disconnect from all signals from layer!
    disconnect( nodeLayer->layer(), nullptr, this, nullptr );
  }
}

void QgsLayerTreeModel::connectToLayers( QgsLayerTreeGroup* parentGroup )
{
  Q_FOREACH ( QgsLayerTreeNode* node, parentGroup->children() )
  {
    if ( QgsLayerTree::isGroup( node ) )
      connectToLayers( QgsLayerTree::toGroup( node ) );
    else if ( QgsLayerTree::isLayer( node ) )
      connectToLayer( QgsLayerTree::toLayer( node ) );
  }
}

void QgsLayerTreeModel::disconnectFromLayers( QgsLayerTreeGroup* parentGroup )
{
  Q_FOREACH ( QgsLayerTreeNode* node, parentGroup->children() )
  {
    if ( QgsLayerTree::isGroup( node ) )
      disconnectFromLayers( QgsLayerTree::toGroup( node ) );
    else if ( QgsLayerTree::isLayer( node ) )
      disconnectFromLayer( QgsLayerTree::toLayer( node ) );
  }
}

void QgsLayerTreeModel::connectToRootNode()
{
  Q_ASSERT( mRootNode );

  connect( mRootNode, SIGNAL( willAddChildren( QgsLayerTreeNode*, int, int ) ), this, SLOT( nodeWillAddChildren( QgsLayerTreeNode*, int, int ) ) );
  connect( mRootNode, SIGNAL( addedChildren( QgsLayerTreeNode*, int, int ) ), this, SLOT( nodeAddedChildren( QgsLayerTreeNode*, int, int ) ) );
  connect( mRootNode, SIGNAL( willRemoveChildren( QgsLayerTreeNode*, int, int ) ), this, SLOT( nodeWillRemoveChildren( QgsLayerTreeNode*, int, int ) ) );
  connect( mRootNode, SIGNAL( removedChildren( QgsLayerTreeNode*, int, int ) ), this, SLOT( nodeRemovedChildren() ) );
  connect( mRootNode, SIGNAL( visibilityChanged( QgsLayerTreeNode*, Qt::CheckState ) ), this, SLOT( nodeVisibilityChanged( QgsLayerTreeNode* ) ) );
  connect( mRootNode, SIGNAL( nameChanged( QgsLayerTreeNode*, QString ) ), this, SLOT( nodeNameChanged( QgsLayerTreeNode*, QString ) ) );

  connect( mRootNode, SIGNAL( customPropertyChanged( QgsLayerTreeNode*, QString ) ), this, SLOT( nodeCustomPropertyChanged( QgsLayerTreeNode*, QString ) ) );

  connectToLayers( mRootNode );
}

void QgsLayerTreeModel::disconnectFromRootNode()
{
  disconnect( mRootNode, nullptr, this, nullptr );

  disconnectFromLayers( mRootNode );
}

void QgsLayerTreeModel::recursivelyEmitDataChanged( const QModelIndex& idx )
{
  QgsLayerTreeNode* node = index2node( idx );
  if ( !node )
    return;

  int count = node->children().count();
  if ( count == 0 )
    return;
  emit dataChanged( index( 0, 0, idx ), index( count - 1, 0, idx ) );
  for ( int i = 0; i < count; ++i )
    recursivelyEmitDataChanged( index( i, 0, idx ) );
}

void QgsLayerTreeModel::refreshScaleBasedLayers( const QModelIndex& idx )
{
  QgsLayerTreeNode* node = index2node( idx );
  if ( !node )
    return;

  if ( node->nodeType() == QgsLayerTreeNode::NodeLayer )
  {
    const QgsMapLayer* layer = QgsLayerTree::toLayer( node )->layer();
    if ( layer && layer->hasScaleBasedVisibility() )
    {
      emit dataChanged( idx, idx );
    }
  }
  int count = node->children().count();
  for ( int i = 0; i < count; ++i )
    refreshScaleBasedLayers( index( i, 0, idx ) );
}

Qt::DropActions QgsLayerTreeModel::supportedDropActions() const
{
  return Qt::CopyAction | Qt::MoveAction;
}

QStringList QgsLayerTreeModel::mimeTypes() const
{
  QStringList types;
  types << "application/qgis.layertreemodeldata";
  return types;
}


QMimeData* QgsLayerTreeModel::mimeData( const QModelIndexList& indexes ) const
{
  // Sort the indexes. Depending on how the user selected the items, the indexes may be unsorted.
  QModelIndexList sortedIndexes = indexes;
  qSort( sortedIndexes.begin(), sortedIndexes.end(), qLess<QModelIndex>() );

  QList<QgsLayerTreeNode*> nodesFinal = indexes2nodes( sortedIndexes, true );

  if ( nodesFinal.isEmpty() )
    return nullptr;

  QMimeData *mimeData = new QMimeData();

  QDomDocument doc;
  QDomElement rootElem = doc.createElement( "layer_tree_model_data" );
  Q_FOREACH ( QgsLayerTreeNode* node, nodesFinal )
    node->writeXML( rootElem );
  doc.appendChild( rootElem );
  QString txt = doc.toString();

  mimeData->setData( "application/qgis.layertreemodeldata", txt.toUtf8() );
  return mimeData;
}

bool QgsLayerTreeModel::dropMimeData( const QMimeData* data, Qt::DropAction action, int row, int column, const QModelIndex& parent )
{
  if ( action == Qt::IgnoreAction )
    return true;

  if ( !data->hasFormat( "application/qgis.layertreemodeldata" ) )
    return false;

  if ( column >= columnCount( parent ) )
    return false;

  QgsLayerTreeNode* nodeParent = index2node( parent );
  if ( !QgsLayerTree::isGroup( nodeParent ) )
    return false;

  QByteArray encodedData = data->data( "application/qgis.layertreemodeldata" );

  QDomDocument doc;
  if ( !doc.setContent( QString::fromUtf8( encodedData ) ) )
    return false;

  QDomElement rootElem = doc.documentElement();
  if ( rootElem.tagName() != "layer_tree_model_data" )
    return false;

  QList<QgsLayerTreeNode*> nodes;

  QDomElement elem = rootElem.firstChildElement();
  while ( !elem.isNull() )
  {
    QgsLayerTreeNode* node = QgsLayerTreeNode::readXML( elem );
    if ( node )
      nodes << node;

    elem = elem.nextSiblingElement();
  }

  if ( nodes.isEmpty() )
    return false;

  if ( parent.isValid() && row == -1 )
    row = 0; // if dropped directly onto group item, insert at first position

  QgsLayerTree::toGroup( nodeParent )->insertChildNodes( row, nodes );

  return true;
}

bool QgsLayerTreeModel::removeRows( int row, int count, const QModelIndex& parent )
{
  QgsLayerTreeNode* parentNode = index2node( parent );
  if ( QgsLayerTree::isGroup( parentNode ) )
  {
    QgsLayerTree::toGroup( parentNode )->removeChildren( row, count );
    return true;
  }
  return false;
}

void QgsLayerTreeModel::setFlags( const QgsLayerTreeModel::Flags& f )
{
  mFlags = f;
}

void QgsLayerTreeModel::setFlag( QgsLayerTreeModel::Flag f, bool on )
{
  if ( on )
    mFlags |= f;
  else
    mFlags &= ~f;
}

QgsLayerTreeModel::Flags QgsLayerTreeModel::flags() const
{
  return mFlags;
}

bool QgsLayerTreeModel::testFlag( QgsLayerTreeModel::Flag f ) const
{
  return mFlags.testFlag( f );
}

const QIcon& QgsLayerTreeModel::iconGroup()
{
  static QIcon icon;

  if ( icon.isNull() )
    icon = QgsApplication::getThemeIcon( "/mActionFolder.svg" );

  return icon;
}

QList<QgsLayerTreeModelLegendNode*> QgsLayerTreeModel::filterLegendNodes( const QList<QgsLayerTreeModelLegendNode*>& nodes )
{
  QList<QgsLayerTreeModelLegendNode*> filtered;

  if ( mLegendFilterByScale > 0 )
  {
    Q_FOREACH ( QgsLayerTreeModelLegendNode* node, nodes )
    {
      if ( node->isScaleOK( mLegendFilterByScale ) )
        filtered << node;
    }
  }
  else if ( mLegendFilterMapSettings )
  {
    if ( !nodes.isEmpty() && mLegendFilterMapSettings->layers().contains( nodes.at( 0 )->layerNode()->layerId() ) )
    {
      Q_FOREACH ( QgsLayerTreeModelLegendNode* node, nodes )
      {
        QgsSymbolV2* ruleKey = reinterpret_cast< QgsSymbolV2* >( node->data( QgsSymbolV2LegendNode::SymbolV2LegacyRuleKeyRole ).value<void*>() );
        bool checked = mLegendFilterUsesExtent || node->data( Qt::CheckStateRole ).toInt() == Qt::Checked;
        if ( ruleKey && checked )
        {
          QString ruleKey = node->data( QgsSymbolV2LegendNode::RuleKeyRole ).toString();
          if ( QgsVectorLayer* vl = qobject_cast<QgsVectorLayer*>( node->layerNode()->layer() ) )
          {
            if ( mLegendFilterHitTest->legendKeyVisible( ruleKey, vl ) )
              filtered << node;
          }
        }
        else  // unknown node type or unchecked
          filtered << node;
      }
    }
  }
  else
  {
    return nodes;
  }

  return filtered;
}



///////////////////////////////////////////////////////////////////////////////
// Legend nodes routines - start

void QgsLayerTreeModel::legendCleanup()
{
  Q_FOREACH ( const LayerLegendData& data, mLegend )
  {
    qDeleteAll( data.originalNodes );
    delete data.tree;
  }
  mLegend.clear();
}


void QgsLayerTreeModel::removeLegendFromLayer( QgsLayerTreeLayer* nodeLayer )
{
  if ( mLegend.contains( nodeLayer ) )
  {
    qDeleteAll( mLegend[nodeLayer].originalNodes );
    delete mLegend[nodeLayer].tree;
    mLegend.remove( nodeLayer );
  }
}


void QgsLayerTreeModel::addLegendToLayer( QgsLayerTreeLayer* nodeL )
{
  if ( !nodeL->layer() )
    return;

  QgsMapLayer* ml = nodeL->layer();
  QgsMapLayerLegend* layerLegend = ml->legend();
  if ( !layerLegend )
    return;

  bool hasStyleOverride = mLayerStyleOverrides.contains( ml->id() );
  if ( hasStyleOverride )
    ml->styleManager()->setOverrideStyle( mLayerStyleOverrides.value( ml->id() ) );

  QList<QgsLayerTreeModelLegendNode*> lstNew = layerLegend->createLayerTreeModelLegendNodes( nodeL );

  // apply filtering defined in layer node's custom properties (reordering, filtering, custom labels)
  QgsMapLayerLegendUtils::applyLayerNodeProperties( nodeL, lstNew );

  if ( testFlag( UseEmbeddedWidgets ) )
  {
    // generate placeholder legend nodes that will be replaced by widgets in QgsLayerTreeView
    int widgetsCount = ml->customProperty( "embeddedWidgets/count", 0 ).toInt();
    while ( widgetsCount > 0 )
    {
      lstNew.insert( 0, new EmbeddedWidgetLegendNode( nodeL ) );
      --widgetsCount;
    }
  }

  QList<QgsLayerTreeModelLegendNode*> filteredLstNew = filterLegendNodes( lstNew );

  Q_FOREACH ( QgsLayerTreeModelLegendNode* n, lstNew )
  {
    n->setParent( this );
    connect( n, SIGNAL( dataChanged() ), this, SLOT( legendNodeDataChanged() ) );
  }

  // See if we have an embedded node - if we do, we will not use it among active nodes.
  // Legend node embedded in parent does not have to be the first one,
  // there can be also nodes generated for embedded widgets
  QgsLayerTreeModelLegendNode* embeddedNode = nullptr;
  Q_FOREACH ( QgsLayerTreeModelLegendNode* legendNode, filteredLstNew )
  {
    if ( legendNode->isEmbeddedInParent() )
    {
      embeddedNode = legendNode;
      filteredLstNew.removeOne( legendNode );
      break;
    }
  }

  LayerLegendTree* legendTree = nullptr;

  // maybe the legend nodes form a tree - try to create a tree structure from the list
  if ( testFlag( ShowLegendAsTree ) )
    legendTree = tryBuildLegendTree( filteredLstNew );

  int count = legendTree ? legendTree->children[nullptr].count() : filteredLstNew.count();

  if ( !filteredLstNew.isEmpty() ) beginInsertRows( node2index( nodeL ), 0, count - 1 );

  LayerLegendData data;
  data.originalNodes = lstNew;
  data.activeNodes = filteredLstNew;
  data.embeddedNodeInParent = embeddedNode;
  data.tree = legendTree;

  mLegend[nodeL] = data;

  if ( !filteredLstNew.isEmpty() ) endInsertRows();

  if ( hasStyleOverride )
    ml->styleManager()->restoreOverrideStyle();

  // invalidate map based data even if the data is not map-based to make sure
  // the symbol sizes are computed at least once
  legendInvalidateMapBasedData();
}


QgsLayerTreeModel::LayerLegendTree* QgsLayerTreeModel::tryBuildLegendTree( const QList<QgsLayerTreeModelLegendNode*>& nodes )
{
  // first check whether there are any legend nodes that are not top-level
  bool hasParentKeys = false;
  Q_FOREACH ( QgsLayerTreeModelLegendNode* n, nodes )
  {
    if ( !n->data( QgsLayerTreeModelLegendNode::ParentRuleKeyRole ).toString().isEmpty() )
    {
      hasParentKeys = true;
      break;
    }
  }
  if ( !hasParentKeys )
    return nullptr; // all legend nodes are top-level => stick with list representation

  // make mapping from rules to nodes and do some sanity checks
  QHash<QString, QgsLayerTreeModelLegendNode*> rule2node;
  rule2node[QString()] = nullptr;
  Q_FOREACH ( QgsLayerTreeModelLegendNode* n, nodes )
  {
    QString ruleKey = n->data( QgsLayerTreeModelLegendNode::RuleKeyRole ).toString();
    if ( ruleKey.isEmpty() ) // in tree all nodes must have key
      return nullptr;
    if ( rule2node.contains( ruleKey ) ) // and they must be unique
      return nullptr;
    rule2node[ruleKey] = n;
  }

  // create the tree structure
  LayerLegendTree* tree = new LayerLegendTree;
  Q_FOREACH ( QgsLayerTreeModelLegendNode* n, nodes )
  {
    QString parentRuleKey = n->data( QgsLayerTreeModelLegendNode::ParentRuleKeyRole ).toString();
    QgsLayerTreeModelLegendNode* parent = rule2node.value( parentRuleKey, nullptr );
    tree->parents[n] = parent;
    tree->children[parent] << n;
  }
  return tree;
}


QgsLayerTreeModelLegendNode* QgsLayerTreeModel::index2legendNode( const QModelIndex& index )
{
  return qobject_cast<QgsLayerTreeModelLegendNode*>( reinterpret_cast<QObject*>( index.internalPointer() ) );
}


QModelIndex QgsLayerTreeModel::legendNode2index( QgsLayerTreeModelLegendNode* legendNode )
{
  const LayerLegendData& data = mLegend[legendNode->layerNode()];
  if ( data.tree )
  {
    if ( QgsLayerTreeModelLegendNode* parentLegendNode = data.tree->parents[legendNode] )
    {
      QModelIndex parentIndex = legendNode2index( parentLegendNode );
      int row = data.tree->children[parentLegendNode].indexOf( legendNode );
      return index( row, 0, parentIndex );
    }
    else
    {
      QModelIndex parentIndex = node2index( legendNode->layerNode() );
      int row = data.tree->children[nullptr].indexOf( legendNode );
      return index( row, 0, parentIndex );
    }
  }

  QModelIndex parentIndex = node2index( legendNode->layerNode() );
  Q_ASSERT( parentIndex.isValid() );
  int row = data.activeNodes.indexOf( legendNode );
  if ( row < 0 ) // legend node may be filtered (exists within the list of original nodes, but not in active nodes)
    return QModelIndex();

  return index( row, 0, parentIndex );
}


int QgsLayerTreeModel::legendNodeRowCount( QgsLayerTreeModelLegendNode* node ) const
{
  const LayerLegendData& data = mLegend[node->layerNode()];
  if ( data.tree )
    return data.tree->children[node].count();

  return 0; // they are leaves
}


int QgsLayerTreeModel::legendRootRowCount( QgsLayerTreeLayer* nL ) const
{
  if ( !mLegend.contains( nL ) )
    return 0;

  const LayerLegendData& data = mLegend[nL];
  if ( data.tree )
    return data.tree->children[nullptr].count();

  int count = data.activeNodes.count();
  return count;
}


QModelIndex QgsLayerTreeModel::legendRootIndex( int row, int column, QgsLayerTreeLayer* nL ) const
{
  Q_ASSERT( mLegend.contains( nL ) );
  const LayerLegendData& data = mLegend[nL];
  if ( data.tree )
    return createIndex( row, column, static_cast<QObject*>( data.tree->children[nullptr].at( row ) ) );

  return createIndex( row, column, static_cast<QObject*>( data.activeNodes.at( row ) ) );
}


QModelIndex QgsLayerTreeModel::legendNodeIndex( int row, int column, QgsLayerTreeModelLegendNode* node ) const
{
  const LayerLegendData& data = mLegend[node->layerNode()];
  if ( data.tree )
    return createIndex( row, column, static_cast<QObject*>( data.tree->children[node].at( row ) ) );

  return QModelIndex(); // have no children
}


QModelIndex QgsLayerTreeModel::legendParent( QgsLayerTreeModelLegendNode* legendNode ) const
{
  QgsLayerTreeLayer* layerNode = legendNode->layerNode();
  const LayerLegendData& data = mLegend[layerNode];
  if ( data.tree )
  {
    if ( QgsLayerTreeModelLegendNode* parentNode = data.tree->parents[legendNode] )
    {
      QgsLayerTreeModelLegendNode* grandParentNode = data.tree->parents[parentNode]; // may be null (not a problem)
      int row = data.tree->children[grandParentNode].indexOf( parentNode );
      return createIndex( row, 0, static_cast<QObject*>( parentNode ) );
    }
    else
      return indexOfParentLayerTreeNode( layerNode );
  }

  return indexOfParentLayerTreeNode( layerNode );
}


QVariant QgsLayerTreeModel::legendNodeData( QgsLayerTreeModelLegendNode* node, int role ) const
{
  if ( role == Qt::CheckStateRole && !testFlag( AllowLegendChangeState ) )
    return QVariant();
  return node->data( role );
}


Qt::ItemFlags QgsLayerTreeModel::legendNodeFlags( QgsLayerTreeModelLegendNode* node ) const
{
  Qt::ItemFlags f = node->flags();
  if ( !testFlag( AllowLegendChangeState ) )
    f &= ~Qt::ItemIsUserCheckable;
  return f;
}


bool QgsLayerTreeModel::legendEmbeddedInParent( QgsLayerTreeLayer* nodeLayer ) const
{
  return mLegend[nodeLayer].embeddedNodeInParent != nullptr;
}

QgsLayerTreeModelLegendNode* QgsLayerTreeModel::legendNodeEmbeddedInParent( QgsLayerTreeLayer* nodeLayer ) const
{
  return mLegend[nodeLayer].embeddedNodeInParent;
}


QIcon QgsLayerTreeModel::legendIconEmbeddedInParent( QgsLayerTreeLayer* nodeLayer ) const
{
  QgsLayerTreeModelLegendNode* legendNode = mLegend[nodeLayer].embeddedNodeInParent;
  if ( !legendNode )
    return QIcon();
  return QIcon( qvariant_cast<QPixmap>( legendNode->data( Qt::DecorationRole ) ) );
}


QList<QgsLayerTreeModelLegendNode*> QgsLayerTreeModel::layerLegendNodes( QgsLayerTreeLayer* nodeLayer, bool skipNodeEmbeddedInParent )
{
  if ( !mLegend.contains( nodeLayer ) )
    return QList<QgsLayerTreeModelLegendNode*>();

  const LayerLegendData& data = mLegend[nodeLayer];
  QList<QgsLayerTreeModelLegendNode*> lst( data.activeNodes );
  if ( !skipNodeEmbeddedInParent && data.embeddedNodeInParent )
    lst.prepend( data.embeddedNodeInParent );
  return lst;
}

QList<QgsLayerTreeModelLegendNode*> QgsLayerTreeModel::layerOriginalLegendNodes( QgsLayerTreeLayer* nodeLayer )
{
  return mLegend.value( nodeLayer ).originalNodes;
}

QgsLayerTreeModelLegendNode* QgsLayerTreeModel::findLegendNode( const QString& layerId, const QString& ruleKey ) const
{
  QMap<QgsLayerTreeLayer*, LayerLegendData>::const_iterator it = mLegend.constBegin();
  for ( ; it != mLegend.constEnd(); ++it )
  {
    QgsLayerTreeLayer* layer = it.key();
    if ( layer->layerId() == layerId )
    {
      Q_FOREACH ( QgsLayerTreeModelLegendNode* legendNode, mLegend.value( layer ).activeNodes )
      {
        if ( legendNode->data( QgsLayerTreeModelLegendNode::RuleKeyRole ).toString() == ruleKey )
        {
          //found it!
          return legendNode;
        }
      }
    }
  }

  return nullptr;
}

void QgsLayerTreeModel::legendInvalidateMapBasedData()
{
  if ( !testFlag( DeferredLegendInvalidation ) )
    invalidateLegendMapBasedData();
  else
    mDeferLegendInvalidationTimer.start( 1000 );
}

void QgsLayerTreeModel::invalidateLegendMapBasedData()
{
  QgsDebugCall;

  // we have varying icon sizes, and we want icon to be centered and
  // text to be left aligned, so we have to compute the max width of icons
  //
  // we do that for nodes who share a common parent
  //
  // we do that here because for symbols with size defined in map units
  // the symbol sizes changes depends on the zoom level

  Q_FOREACH ( const LayerLegendData& data, mLegend )
  {
    QList<QgsSymbolV2LegendNode*> symbolNodes;
    QMap<QString, int> widthMax;
    Q_FOREACH ( QgsLayerTreeModelLegendNode* legendNode, data.originalNodes )
    {
      QgsSymbolV2LegendNode* n = dynamic_cast<QgsSymbolV2LegendNode*>( legendNode );
      if ( n )
      {
        const QSize sz( n->minimumIconSize() );
        const QString parentKey( n->data( QgsLayerTreeModelLegendNode::ParentRuleKeyRole ).toString() );
        widthMax[parentKey] = qMax( sz.width(), widthMax.contains( parentKey ) ? widthMax[parentKey] : 0 );
        n->setIconSize( sz );
        symbolNodes.append( n );
      }
    }
    Q_FOREACH ( QgsSymbolV2LegendNode* n, symbolNodes )
    {
      const QString parentKey( n->data( QgsLayerTreeModelLegendNode::ParentRuleKeyRole ).toString() );
      Q_ASSERT( widthMax[parentKey] > 0 );
      const int twiceMarginWidth = 2; // a one pixel margin avoids hugly rendering of icon
      n->setIconSize( QSize( widthMax[parentKey] + twiceMarginWidth, n->iconSize().rheight() + twiceMarginWidth ) );
    }
    Q_FOREACH ( QgsLayerTreeModelLegendNode* legendNode, data.originalNodes )
      legendNode->invalidateMapBasedData();
  }

}

// Legend nodes routines - end
///////////////////////////////////////////////////////////////////////////////
