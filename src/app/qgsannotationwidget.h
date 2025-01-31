/***************************************************************************
                              qgsannotationwidget.h
                              ------------------------
  begin                : February 25, 2010
  copyright            : (C) 2010 by Marco Hugentobler
  email                : marco dot hugentobler at hugis dot net
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#ifndef QGSANNOTATIONWIDGET_H
#define QGSANNOTATIONWIDGET_H

#include "ui_qgsannotationwidgetbase.h"

class QgsAnnotationItem;
class QgsMarkerSymbolV2;

/** A configuration widget to configure the annotation item properties. Usually embedded by QgsAnnotationItem
subclass configuration dialogs*/
class APP_EXPORT QgsAnnotationWidget: public QWidget, private Ui::QgsAnnotationWidgetBase
{
    Q_OBJECT
  public:
    QgsAnnotationWidget( QgsAnnotationItem* item, QWidget * parent = nullptr, Qt::WindowFlags f = nullptr );
    ~QgsAnnotationWidget();

    void apply();

  signals:

    //! Emitted when the background color of the annotation is changed
    void backgroundColorChanged( const QColor& color );

  private slots:
    void on_mMapMarkerButton_clicked();

  private:
    QgsAnnotationItem* mItem;
    QScopedPointer< QgsMarkerSymbolV2 > mMarkerSymbol;

    void blockAllSignals( bool block );
    void updateCenterIcon();
};

#endif // QGSANNOTATIONWIDGET_H
