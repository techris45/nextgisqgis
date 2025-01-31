/***************************************************************************
    qgsrelationreferencewidgetwrapper.cpp
     --------------------------------------
    Date                 : 20.4.2013
    Copyright            : (C) 2013 Matthias Kuhn
    Email                : matthias at opengis dot ch
 ***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/


#include "qgsrelationreferencewidgetwrapper.h"
#include "qgsproject.h"
#include "qgsrelationmanager.h"


QgsRelationReferenceWidgetWrapper::QgsRelationReferenceWidgetWrapper( QgsVectorLayer* vl, int fieldIdx, QWidget* editor, QgsMapCanvas* canvas, QgsMessageBar* messageBar, QWidget* parent )
    : QgsEditorWidgetWrapper( vl, fieldIdx, editor, parent )
    , mWidget( nullptr )
    , mCanvas( canvas )
    , mMessageBar( messageBar )
    , mIndeterminateState( false )
{
}

QWidget* QgsRelationReferenceWidgetWrapper::createWidget( QWidget* parent )
{
  QgsRelationReferenceWidget* w = new QgsRelationReferenceWidget( parent );
  return w;
}

void QgsRelationReferenceWidgetWrapper::initWidget( QWidget* editor )
{
  QgsRelationReferenceWidget* w = dynamic_cast<QgsRelationReferenceWidget*>( editor );
  if ( !w )
  {
    w = new QgsRelationReferenceWidget( editor );
  }

  mWidget = w;

  mWidget->setEditorContext( context(), mCanvas, mMessageBar );

  bool showForm = config( "ShowForm", true ).toBool();
  bool mapIdent = config( "MapIdentification", false ).toBool();
  bool readOnlyWidget = config( "ReadOnly", false ).toBool();
  bool orderByValue = config( "OrderByValue", false ).toBool();

  mWidget->setEmbedForm( showForm );
  mWidget->setReadOnlySelector( readOnlyWidget );
  mWidget->setAllowMapIdentification( mapIdent );
  mWidget->setOrderByValue( orderByValue );
  if ( config( "FilterFields", QVariant() ).isValid() )
  {
    mWidget->setFilterFields( config( "FilterFields" ).toStringList() );
    mWidget->setChainFilters( config( "ChainFilters" ).toBool() );
  }
  mWidget->setAllowAddFeatures( config( "AllowAddFeatures", false ).toBool() );

  QgsRelation relation = QgsProject::instance()->relationManager()->relation( config( "Relation" ).toString() );

  // If this widget is already embedded by the same relation, reduce functionality
  const QgsAttributeEditorContext* ctx = &context();
  do
  {
    if ( ctx->relation().id() == relation.id() )
    {
      mWidget->setEmbedForm( false );
      mWidget->setReadOnlySelector( true );
      mWidget->setAllowMapIdentification( false );
    }
    ctx = ctx->parentContext();
  }
  while ( ctx );

  mWidget->setRelation( relation, config( "AllowNULL" ).toBool() );

  connect( mWidget, SIGNAL( foreignKeyChanged( QVariant ) ), this,  SLOT( foreignKeyChanged( QVariant ) ) );
}

QVariant QgsRelationReferenceWidgetWrapper::value() const
{
  if ( !mWidget )
    return QVariant( field().type() );

  QVariant v = mWidget->foreignKey();

  if ( v.isNull() )
  {
    return QVariant( field().type() );
  }
  else
  {
    return v;
  }
}

bool QgsRelationReferenceWidgetWrapper::valid() const
{
  return mWidget;
}

void QgsRelationReferenceWidgetWrapper::showIndeterminateState()
{
  if ( mWidget )
  {
    mWidget->showIndeterminateState();
  }
  mIndeterminateState = true;
}

void QgsRelationReferenceWidgetWrapper::setValue( const QVariant& val )
{
  if ( !mWidget || ( !mIndeterminateState && val == value() ) )
    return;

  mIndeterminateState = false;
  mWidget->setForeignKey( val );
}

void QgsRelationReferenceWidgetWrapper::setEnabled( bool enabled )
{
  if ( !mWidget )
    return;

  mWidget->setRelationEditable( enabled );
}

void QgsRelationReferenceWidgetWrapper::foreignKeyChanged( QVariant value )
{
  if ( !value.isValid() || value.isNull() )
  {
    value = QVariant( field().type() );
  }
  emit valueChanged( value );
}

void QgsRelationReferenceWidgetWrapper::updateConstraintWidgetStatus( bool constraintValid )
{
  if ( mWidget )
  {
    if ( constraintValid )
      mWidget->setStyleSheet( QString() );
    else
      mWidget->setStyleSheet( ".QComboBox { background-color: #dd7777; }" );
  }
}
