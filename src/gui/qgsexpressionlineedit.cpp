/***************************************************************************
   qgsexpressionlineedit.cpp
    ------------------------
   Date                 : 18.08.2016
   Copyright            : (C) 2016 Nyall Dawson
   Email                : nyall dot dawson at gmail dot com
***************************************************************************
*                                                                         *
*   This program is free software; you can redistribute it and/or modify  *
*   it under the terms of the GNU General Public License as published by  *
*   the Free Software Foundation; either version 2 of the License, or     *
*   (at your option) any later version.                                   *
*                                                                         *
***************************************************************************/

#include "qgsexpressionlineedit.h"
#include "qgsfilterlineedit.h"
#include "qgsexpressioncontext.h"
#include "qgsapplication.h"
#include "qgsexpressionbuilderdialog.h"
#include "qgscodeeditorsql.h"
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QToolButton>


QgsExpressionLineEdit::QgsExpressionLineEdit( QWidget *parent )
    : QWidget( parent )
    , mLineEdit( nullptr )
    , mCodeEditor( nullptr )
    , mExpressionDialogTitle( tr( "Expression dialog" ) )
    , mDa( nullptr )
    , mLayer( nullptr )
{
  mButton = new QToolButton();
  mButton->setSizePolicy( QSizePolicy::Minimum, QSizePolicy::Minimum );
  mButton->setIcon( QgsApplication::getThemeIcon( "/mIconExpression.svg" ) );
  connect( mButton, SIGNAL( clicked() ), this, SLOT( editExpression() ) );

  //sets up layout
  setMultiLine( false );

  mExpressionContext = QgsExpressionContext();
  mExpressionContext << QgsExpressionContextUtils::globalScope()
  << QgsExpressionContextUtils::projectScope();
}

void QgsExpressionLineEdit::setExpressionDialogTitle( const QString& title )
{
  mExpressionDialogTitle = title;
}

void QgsExpressionLineEdit::setMultiLine( bool multiLine )
{
  QString exp = expression();

  if ( multiLine && !mCodeEditor )
  {
    mCodeEditor = new QgsCodeEditorSQL();
    mCodeEditor->setSizePolicy( QSizePolicy::Expanding, QSizePolicy::Expanding );
    delete mLineEdit;
    mLineEdit = nullptr;

    QHBoxLayout* newLayout = new QHBoxLayout();
    newLayout->setContentsMargins( 0, 0, 0, 0 );
    newLayout->addWidget( mCodeEditor );

    QVBoxLayout* vLayout = new QVBoxLayout();
    vLayout->addWidget( mButton );
    vLayout->addStretch();
    newLayout->addLayout( vLayout );

    delete layout();
    setLayout( newLayout );

    setSizePolicy( QSizePolicy::Expanding, QSizePolicy::Expanding );

    setFocusProxy( mCodeEditor );
    connect( mCodeEditor, SIGNAL( textChanged() ), this, SLOT( expressionEdited() ) );

    setExpression( exp );
  }
  else if ( !multiLine && !mLineEdit )
  {
    delete mCodeEditor;
    mCodeEditor = nullptr;
    mLineEdit = new QgsFilterLineEdit();
    mLineEdit->setSizePolicy( QSizePolicy::Expanding, QSizePolicy::Minimum );

    QHBoxLayout* newLayout = new QHBoxLayout();
    newLayout->setContentsMargins( 0, 0, 0, 0 );
    newLayout->addWidget( mLineEdit );
    newLayout->addWidget( mButton );

    delete layout();
    setLayout( newLayout );

    setSizePolicy( QSizePolicy::Expanding, QSizePolicy::Minimum );

    setFocusProxy( mLineEdit );
    connect( mLineEdit, SIGNAL( textChanged( QString ) ), this, SLOT( expressionEdited( QString ) ) );

    setExpression( exp );
  }
}

void QgsExpressionLineEdit::setGeomCalculator( const QgsDistanceArea &da )
{
  mDa.reset( new QgsDistanceArea( da ) );
}

void QgsExpressionLineEdit::setLayer( QgsVectorLayer* layer )
{
  mLayer = layer;
}

QString QgsExpressionLineEdit::expression() const
{
  if ( mLineEdit )
    return mLineEdit->text();
  else if ( mCodeEditor )
    return mCodeEditor->text();

  return QString();
}

bool QgsExpressionLineEdit::isValidExpression( QString *expressionError ) const
{
  QString temp;
  return QgsExpression::isValid( expression(), &mExpressionContext, expressionError ? *expressionError : temp );
}

void QgsExpressionLineEdit::setExpression( const QString& newExpression )
{
  if ( mLineEdit )
    mLineEdit->setText( newExpression );
  else if ( mCodeEditor )
    mCodeEditor->setText( newExpression );
}

void QgsExpressionLineEdit::editExpression()
{
  QString currentExpression = expression();

  QgsExpressionContext context = mExpressionContext;

  QgsExpressionBuilderDialog dlg( mLayer, currentExpression, this, "generic", context );
  if ( !mDa.isNull() )
  {
    dlg.setGeomCalculator( *mDa );
  }
  dlg.setWindowTitle( mExpressionDialogTitle );

  if ( dlg.exec() )
  {
    QString newExpression = dlg.expressionText();
    setExpression( newExpression );
  }
}

void QgsExpressionLineEdit::expressionEdited()
{
  emit expressionChanged( expression() );
}

void QgsExpressionLineEdit::expressionEdited( const QString& expression )
{
  updateLineEditStyle( expression );
  emit expressionChanged( expression );
}

void QgsExpressionLineEdit::changeEvent( QEvent* event )
{
  if ( event->type() == QEvent::EnabledChange )
  {
    updateLineEditStyle( expression() );
  }
}

void QgsExpressionLineEdit::updateLineEditStyle( const QString& expression )
{
  if ( !mLineEdit )
    return;

  QPalette palette;
  if ( !isEnabled() )
  {
    palette.setColor( QPalette::Text, Qt::gray );
  }
  else
  {
    bool isValid = true;
    if ( !expression.isEmpty() )
    {
      isValid = isExpressionValid( expression );
    }
    if ( !isValid )
    {
      palette.setColor( QPalette::Text, Qt::red );
    }
    else
    {
      palette.setColor( QPalette::Text, Qt::black );
    }
  }
  mLineEdit->setPalette( palette );
}

bool QgsExpressionLineEdit::isExpressionValid( const QString& expressionStr )
{
  QgsExpression expression( expressionStr );
  expression.prepare( &mExpressionContext );
  return !expression.hasParserError();
}
