/***************************************************************************
    qgsdatetimeeditwrapper.cpp
     --------------------------------------
    Date                 : 03.2014
    Copyright            : (C) 2014 Denis Rouzaud
    Email                : denis.rouzaud@gmail.com
 ***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include "qgsdatetimeeditwrapper.h"
#include "qgsdatetimeeditfactory.h"
#include "qgsmessagelog.h"
#include "qgslogger.h"

#include <QDateTimeEdit>
#include <QDateEdit>
#include <QTimeEdit>
#include <QTextCharFormat>
#include <QCalendarWidget>

QgsDateTimeEditWrapper::QgsDateTimeEditWrapper( QgsVectorLayer* vl, int fieldIdx, QWidget* editor, QWidget* parent )
    : QgsEditorWidgetWrapper( vl, fieldIdx, editor, parent )
    , mQDateTimeEdit( nullptr )
    , mQgsDateTimeEdit( nullptr )
{
}

QWidget *QgsDateTimeEditWrapper::createWidget( QWidget *parent )
{
  QgsDateTimeEdit* widget = new QgsDateTimeEdit( parent );
  widget->setDateTime( QDateTime::currentDateTime() );
  return widget;
}

void QgsDateTimeEditWrapper::initWidget( QWidget *editor )
{
  QgsDateTimeEdit* qgsEditor = dynamic_cast<QgsDateTimeEdit*>( editor );
  if ( qgsEditor )
  {
    mQgsDateTimeEdit = qgsEditor;
  }
  // assign the Qt editor also if the QGIS editor has been previously assigned
  // this avoids testing each time which widget to use
  // the QGIS editor must be used for non-virtual methods (dateTime, setDateTime)
  QDateTimeEdit* qtEditor = dynamic_cast<QDateTimeEdit*>( editor );
  if ( qtEditor )
  {
    mQDateTimeEdit = qtEditor;
  }

  if ( !mQDateTimeEdit )
  {
    QgsDebugMsg( "Date/time edit widget could not be initialized because provided widget is not a QDateTimeEdit." );
    QgsMessageLog::logMessage( "Date/time edit widget could not be initialized because provided widget is not a QDateTimeEdit.", "UI forms", QgsMessageLog::WARNING );
    return;
  }

  const QString displayFormat = config( "display_format", QGSDATETIMEEDIT_DATEFORMAT ).toString();
  mQDateTimeEdit->setDisplayFormat( displayFormat );

  const bool calendar = config( "calendar_popup", false ).toBool();

  if ( calendar != mQDateTimeEdit->calendarPopup() )
  {
    mQDateTimeEdit->setCalendarPopup( calendar );
  }
  if ( calendar && mQDateTimeEdit->calendarWidget() )
  {
    // highlight today's date
    QTextCharFormat todayFormat;
    todayFormat.setBackground( QColor( 160, 180, 200 ) );
    mQDateTimeEdit->calendarWidget()->setDateTextFormat( QDate::currentDate(), todayFormat );
  }

  const bool allowNull = config( "allow_null", true ).toBool();
  if ( mQgsDateTimeEdit )
  {
    mQgsDateTimeEdit->setAllowNull( allowNull );
  }
  else
  {
    QgsMessageLog::instance()->logMessage( tr( "The usual date/time widget QDateTimeEdit cannot be configured to allow NULL values. "
                                           "For that the QGIS custom widget QgsDateTimeEdit needs to be used." ),
                                           "field widgets" );
  }

  if ( mQgsDateTimeEdit )
  {
    connect( mQgsDateTimeEdit, SIGNAL( dateTimeChanged( QDateTime ) ), this, SLOT( dateTimeChanged( QDateTime ) ) );
  }
  else
  {
    connect( mQDateTimeEdit, SIGNAL( dateTimeChanged( QDateTime ) ), this,  SLOT( dateTimeChanged( QDateTime ) ) );
  }
}

bool QgsDateTimeEditWrapper::valid() const
{
  return mQgsDateTimeEdit || mQDateTimeEdit;
}

void QgsDateTimeEditWrapper::showIndeterminateState()
{
  if ( mQgsDateTimeEdit )
    mQgsDateTimeEdit->setEmpty();
}

void QgsDateTimeEditWrapper::dateTimeChanged( const QDateTime& dateTime )
{
  switch ( field().type() )
  {
    case QVariant::DateTime:
      emit valueChanged( dateTime );
      break;
    case QVariant::Date:
      emit valueChanged( dateTime.date() );
      break;
    case QVariant::Time:
      emit valueChanged( dateTime.time() );
      break;
    default:
      const bool fieldIsoFormat = config( "field_iso_format" , false ).toBool();
      const QString fieldFormat = config( "field_format" , QGSDATETIMEEDIT_DATEFORMAT ).toString();
      if ( fieldIsoFormat )
      {
        emit valueChanged( dateTime.toString( Qt::ISODate ) );
      }
      else
      {
        emit valueChanged( dateTime.toString( fieldFormat ) );
      }
      break;
  }
}

QVariant QgsDateTimeEditWrapper::value() const
{
  if ( !mQDateTimeEdit )
    return QVariant( field().type() );

  QDateTime dateTime;
  if ( mQgsDateTimeEdit )
  {
    dateTime = mQgsDateTimeEdit->dateTime();
  }
  else
  {
    dateTime = mQDateTimeEdit->dateTime();
  }

  switch ( field().type() )
  {
    case QVariant::DateTime:
      return dateTime;
      break;
    case QVariant::Date:
      return dateTime.date();
      break;
    case QVariant::Time:
      return dateTime.time();
      break;
    default:
      const bool fieldIsoFormat = config( "field_iso_format", false ).toBool();
      const QString fieldFormat = config( "field_format", QGSDATETIMEEDIT_DATEFORMAT ).toString();
      if ( fieldIsoFormat )
      {
        return dateTime.toString( Qt::ISODate );
      }
      else
      {
        return dateTime.toString( fieldFormat );
      }
      break;
  }
  return QVariant();
}

void QgsDateTimeEditWrapper::setValue( const QVariant &value )
{
  if ( !mQDateTimeEdit )
    return;


  QDateTime dateTime;
  switch ( field().type() )
  {
    case QVariant::DateTime:
    case QVariant::Date:
    case QVariant::Time:
      dateTime = value.toDateTime();
      break;
    default:
      const bool fieldIsoFormat = config( "field_iso_format", false ).toBool();
      const QString fieldFormat = config( "field_format", QGSDATETIMEEDIT_DATEFORMAT ).toString();
      if ( fieldIsoFormat )
      {
        dateTime = QDateTime::fromString( value.toString(), Qt::ISODate );
      }
      else
      {
        dateTime = QDateTime::fromString( value.toString(), fieldFormat );
      }
      break;
  }

  if ( mQgsDateTimeEdit )
  {
    mQgsDateTimeEdit->setDateTime( dateTime );
  }
  else
  {
    mQDateTimeEdit->setDateTime( dateTime );
  }
}

void QgsDateTimeEditWrapper::setEnabled( bool enabled )
{
  if ( !mQDateTimeEdit )
    return;

  mQDateTimeEdit->setEnabled( enabled );
}
