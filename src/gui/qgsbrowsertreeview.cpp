/***************************************************************************
  qgsbrowsertreeview.cpp
  --------------------------------------
  Date                 : January 2015
  Copyright            : (C) 2015 by Radim Blazek
  Email                : radim.blazek@gmail.com
 ***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include <QSettings>

#include "qgsbrowsermodel.h"
#include "qgsbrowsertreeview.h"

#include "qgslogger.h"


QgsBrowserTreeView::QgsBrowserTreeView( QWidget *parent )
    : QTreeView( parent )
    , mSettingsSection( "browser" )
    , mBrowserModel( nullptr )
{
}

QgsBrowserTreeView::~QgsBrowserTreeView()
{
}

void QgsBrowserTreeView::setBrowserModel( QgsBrowserModel *model )
{
  mBrowserModel = model;
}

void QgsBrowserTreeView::setModel( QAbstractItemModel* model )
{

  QTreeView::setModel( model );

  restoreState();
}

void QgsBrowserTreeView::showEvent( QShowEvent * e )
{
  Q_UNUSED( e );
  if ( model() )
    restoreState();
  QTreeView::showEvent( e );
}

// closeEvent is not called when application is closed
void QgsBrowserTreeView::hideEvent( QHideEvent * e )
{
  Q_UNUSED( e );
  // hideEvent() may be called (Mac) before showEvent
  if ( model() )
    saveState();
  QTreeView::hideEvent( e );
}

void QgsBrowserTreeView::saveState()
{
  QSettings settings;
  QStringList expandedPaths = expandedPathsList( QModelIndex() );
  settings.setValue( expandedPathsKey(), expandedPaths );
  QgsDebugMsg( "expandedPaths = " + expandedPaths.join( " " ) );
}

void QgsBrowserTreeView::restoreState()
{
  QSettings settings;
  mExpandPaths = settings.value( expandedPathsKey(), QVariant() ).toStringList();

  QgsDebugMsg( "mExpandPaths = " + mExpandPaths.join( " " ) );
  if ( !mExpandPaths.isEmpty() )
  {
    QSet<QModelIndex> expandIndexSet;
    Q_FOREACH ( const QString& path, mExpandPaths )
    {
      QModelIndex expandIndex = QgsBrowserModel::findPath( model(), path, Qt::MatchStartsWith );
      if ( expandIndex.isValid() )
      {
        QModelIndex modelIndex = browserModel()->findPath( path, Qt::MatchExactly );
        if ( modelIndex.isValid() )
        {
          QgsDataItem *ptr = browserModel()->dataItem( modelIndex );
          if ( ptr && ( ptr->capabilities2() & QgsDataItem::Capability::Collapse ) )
          {
            QgsDebugMsgLevel( "do not expand index for path " + path, 4 );
            QModelIndex parentIndex = model()->parent( expandIndex );
            // Still we need to store the parent in order to expand it
            if ( parentIndex.isValid() )
              expandIndexSet.insert( parentIndex );
          }
          else
          {
            expandIndexSet.insert( expandIndex );
          }
        }

      }
      else
      {
        QgsDebugMsg( "index for path " + path + " not found" );
      }
    }
    Q_FOREACH ( const QModelIndex& expandIndex, expandIndexSet )
    {
      expandTree( expandIndex );
    }
  }
  else
  {
    // expand root favourites item
    QModelIndex index = QgsBrowserModel::findPath( model(), "favourites:" );
    expand( index );
  }
}

void QgsBrowserTreeView::expandTree( const QModelIndex & index )
{
  if ( !model() )
    return;

  QgsDebugMsg( "itemPath = " + model()->data( index, QgsBrowserModel::PathRole ).toString() );

  expand( index );
  QModelIndex parentIndex = model()->parent( index );
  if ( parentIndex.isValid() )
    expandTree( parentIndex );
}

bool QgsBrowserTreeView::treeExpanded( const QModelIndex & index )
{
  if ( !model() )
    return false;
  if ( !isExpanded( index ) )
    return false;
  QModelIndex parentIndex = model()->parent( index );
  if ( parentIndex.isValid() )
    return treeExpanded( parentIndex );

  return true; // root
}

bool QgsBrowserTreeView::hasExpandedDescendant( const QModelIndex& index ) const
{
  if ( !model() )
    return false;

  for ( int i = 0 ; i < model()->rowCount( index ); i++ )
  {
    QModelIndex childIndex = model()->index( i, 0, index );
    if ( isExpanded( childIndex ) )
      return true;

    if ( hasExpandedDescendant( childIndex ) )
      return true;
  }
  return false;
}

// rowsInserted signal is used to continue in state restoring
void QgsBrowserTreeView::rowsInserted( const QModelIndex & parentIndex, int start, int end )
{
  QTreeView::rowsInserted( parentIndex, start, end );

  if ( !model() )
    return;

  if ( mExpandPaths.isEmpty() )
    return;

  QgsDebugMsgLevel( "mExpandPaths = " + mExpandPaths.join( "," ), 2 );

  QString parentPath = model()->data( parentIndex, QgsBrowserModel::PathRole ).toString();
  QgsDebugMsgLevel( "parentPath = " + parentPath, 2 );

  // remove parentPath from paths to be expanded
  mExpandPaths.removeOne( parentPath );

  // Remove the subtree from mExpandPaths if user collapsed the item in the meantime
  if ( !treeExpanded( parentIndex ) )
  {
    Q_FOREACH ( const QString& path, mExpandPaths )
    {
      if ( path.startsWith( parentPath + '/' ) )
        mExpandPaths.removeOne( path );
    }
    return;
  }

  for ( int i = start; i <= end; i++ )
  {
    QModelIndex childIndex = model()->index( i, 0, parentIndex );
    QString childPath = model()->data( childIndex, QgsBrowserModel::PathRole ).toString();
    QString escapedChildPath = childPath;
    escapedChildPath.replace( '|', "\\|" );

    QgsDebugMsgLevel( "childPath = " + childPath + " escapedChildPath = " + escapedChildPath, 2 );
    if ( mExpandPaths.contains( childPath ) || mExpandPaths.indexOf( QRegExp( "^" + escapedChildPath + "/.*" ) ) != -1 )
    {
      QgsDebugMsgLevel( "-> expand", 2 );
      expand( childIndex );
    }
  }
}

QString QgsBrowserTreeView::expandedPathsKey() const
{
  return '/' + mSettingsSection + "/expandedPaths";
}

QStringList QgsBrowserTreeView::expandedPathsList( const QModelIndex & index )
{
  QStringList paths;

  if ( !model() )
    return paths;

  for ( int i = 0; i < model()->rowCount( index ); i++ )
  {
    QModelIndex childIndex = model()->index( i, 0, index );
    if ( isExpanded( childIndex ) )
    {
      QStringList childrenPaths = expandedPathsList( childIndex );
      if ( !childrenPaths.isEmpty() )
      {
        paths.append( childrenPaths );
      }
      else
      {
        paths.append( model()->data( childIndex, QgsBrowserModel::PathRole ).toString() );
      }
    }
  }
  return paths;
}
