/***************************************************************************
                            qgscomposerscalebarwidget.cpp
                            -----------------------------
    begin                : 11 June 2008
    copyright            : (C) 2008 by Marco Hugentobler
    email                : marco dot hugentobler at karto dot baug dot ethz dot ch
 ***************************************************************************/
/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include "qgscomposerscalebarwidget.h"
#include "qgscomposeritemwidget.h"
#include "qgscomposermap.h"
#include "qgscomposerscalebar.h"
#include "qgscomposition.h"
#include <QColorDialog>
#include <QFontDialog>
#include <QWidget>

QgsComposerScaleBarWidget::QgsComposerScaleBarWidget( QgsComposerScaleBar* scaleBar ): QgsComposerItemBaseWidget( nullptr, scaleBar ), mComposerScaleBar( scaleBar )
{
  setupUi( this );
  connectUpdateSignal();

  //add widget for general composer item properties
  QgsComposerItemWidget* itemPropertiesWidget = new QgsComposerItemWidget( this, scaleBar );
  mainLayout->addWidget( itemPropertiesWidget );

  mSegmentSizeRadioGroup.addButton( mFixedSizeRadio );
  mSegmentSizeRadioGroup.addButton( mFitWidthRadio );
  connect( &mSegmentSizeRadioGroup, SIGNAL( buttonClicked( QAbstractButton* ) ), this, SLOT( segmentSizeRadioChanged( QAbstractButton* ) ) );

  blockMemberSignals( true );

  //style combo box
  mStyleComboBox->insertItem( 0, tr( "Single Box" ) );
  mStyleComboBox->insertItem( 1, tr( "Double Box" ) );
  mStyleComboBox->insertItem( 2, tr( "Line Ticks Middle" ) );
  mStyleComboBox->insertItem( 3, tr( "Line Ticks Down" ) );
  mStyleComboBox->insertItem( 4, tr( "Line Ticks Up" ) );
  mStyleComboBox->insertItem( 5, tr( "Numeric" ) );

  //alignment combo box
  mAlignmentComboBox->insertItem( 0, tr( "Left" ) );
  mAlignmentComboBox->insertItem( 1, tr( "Middle" ) );
  mAlignmentComboBox->insertItem( 2, tr( "Right" ) );

  //units combo box
  mUnitsComboBox->insertItem( 0, tr( "Map units" ), 0 );
  mUnitsComboBox->insertItem( 1, tr( "Meters" ), 1 );
  mUnitsComboBox->insertItem( 2, tr( "Feet" ), 2 );
  mUnitsComboBox->insertItem( 3, tr( "Nautical Miles" ), 3 );

  mFillColorButton->setColorDialogTitle( tr( "Select fill color" ) );
  mFillColorButton->setAllowAlpha( true );
  mFillColorButton->setContext( "composer" );
  mFillColorButton->setNoColorString( tr( "Transparent fill" ) );
  mFillColorButton->setShowNoColor( true );

  mFillColor2Button->setColorDialogTitle( tr( "Select alternate fill color" ) );
  mFillColor2Button->setAllowAlpha( true );
  mFillColor2Button->setContext( "composer" );
  mFillColor2Button->setNoColorString( tr( "Transparent fill" ) );
  mFillColor2Button->setShowNoColor( true );

  mFontColorButton->setColorDialogTitle( tr( "Select font color" ) );
  mFontColorButton->setAllowAlpha( true );
  mFontColorButton->setContext( "composer" );

  mStrokeColorButton->setColorDialogTitle( tr( "Select line color" ) );
  mStrokeColorButton->setAllowAlpha( true );
  mStrokeColorButton->setContext( "composer" );
  mStrokeColorButton->setNoColorString( tr( "Transparent line" ) );
  mStrokeColorButton->setShowNoColor( true );

  QgsComposition* scaleBarComposition = mComposerScaleBar->composition();
  if ( scaleBarComposition )
  {
    mMapItemComboBox->setComposition( scaleBarComposition );
    mMapItemComboBox->setItemType( QgsComposerItem::ComposerMap );
  }

  connect( mMapItemComboBox, SIGNAL( itemChanged( QgsComposerItem* ) ), this, SLOT( composerMapChanged( QgsComposerItem* ) ) );

  blockMemberSignals( false );
  setGuiElements(); //set the GUI elements to the state of scaleBar
}

QgsComposerScaleBarWidget::~QgsComposerScaleBarWidget()
{

}

void QgsComposerScaleBarWidget::setGuiElements()
{
  if ( !mComposerScaleBar )
  {
    return;
  }

  blockMemberSignals( true );
  mNumberOfSegmentsSpinBox->setValue( mComposerScaleBar->numSegments() );
  mSegmentsLeftSpinBox->setValue( mComposerScaleBar->numSegmentsLeft() );
  mSegmentSizeSpinBox->setValue( mComposerScaleBar->numUnitsPerSegment() );
  mLineWidthSpinBox->setValue( mComposerScaleBar->pen().widthF() );
  mHeightSpinBox->setValue( mComposerScaleBar->height() );
  mMapUnitsPerBarUnitSpinBox->setValue( mComposerScaleBar->numMapUnitsPerScaleBarUnit() );
  mLabelBarSpaceSpinBox->setValue( mComposerScaleBar->labelBarSpace() );
  mBoxSizeSpinBox->setValue( mComposerScaleBar->boxContentSpace() );
  mUnitLabelLineEdit->setText( mComposerScaleBar->unitLabeling() );
  mLineJoinStyleCombo->setPenJoinStyle( mComposerScaleBar->lineJoinStyle() );
  mLineCapStyleCombo->setPenCapStyle( mComposerScaleBar->lineCapStyle() );
  mFontColorButton->setColor( mComposerScaleBar->fontColor() );
  mFillColorButton->setColor( mComposerScaleBar->brush().color() );
  mFillColor2Button->setColor( mComposerScaleBar->brush2().color() );
  mStrokeColorButton->setColor( mComposerScaleBar->pen().color() );

  //map combo box
  mMapItemComboBox->setItem( mComposerScaleBar->composerMap() );

  //style...
  QString style = mComposerScaleBar->style();
  mStyleComboBox->setCurrentIndex( mStyleComboBox->findText( tr( style.toLocal8Bit().data() ) ) );
  toggleStyleSpecificControls( style );

  //alignment
  mAlignmentComboBox->setCurrentIndex(( int )( mComposerScaleBar->alignment() ) );

  //units
  mUnitsComboBox->setCurrentIndex( mUnitsComboBox->findData(( int )mComposerScaleBar->units() ) );

  if ( mComposerScaleBar->segmentSizeMode() == QgsComposerScaleBar::SegmentSizeFixed )
  {
    mFixedSizeRadio->setChecked( true );
    mSegmentSizeSpinBox->setEnabled( true );
    mMinWidthSpinBox->setEnabled( false );
    mMaxWidthSpinBox->setEnabled( false );
  }
  else /*if(mComposerScaleBar->segmentSizeMode() == QgsComposerScaleBar::SegmentSizeFitWidth)*/
  {
    mFitWidthRadio->setChecked( true );
    mSegmentSizeSpinBox->setEnabled( false );
    mMinWidthSpinBox->setEnabled( true );
    mMaxWidthSpinBox->setEnabled( true );
  }
  mMinWidthSpinBox->setValue( mComposerScaleBar->minBarWidth() );
  mMaxWidthSpinBox->setValue( mComposerScaleBar->maxBarWidth() );

  blockMemberSignals( false );
}

//slots

void QgsComposerScaleBarWidget::on_mLineWidthSpinBox_valueChanged( double d )
{
  if ( !mComposerScaleBar )
  {
    return;
  }

  mComposerScaleBar->beginCommand( tr( "Scalebar line width" ), QgsComposerMergeCommand::ScaleBarLineWidth );
  disconnectUpdateSignal();
  QPen newPen = mComposerScaleBar->pen();
  newPen.setWidthF( d );
  mComposerScaleBar->setPen( newPen );
  mComposerScaleBar->update();
  connectUpdateSignal();
  mComposerScaleBar->endCommand();
}

void QgsComposerScaleBarWidget::on_mSegmentSizeSpinBox_valueChanged( double d )
{
  if ( !mComposerScaleBar )
  {
    return;
  }

  mComposerScaleBar->beginCommand( tr( "Scalebar segment size" ), QgsComposerMergeCommand::ScaleBarSegmentSize );
  disconnectUpdateSignal();
  mComposerScaleBar->setNumUnitsPerSegment( d );
  mComposerScaleBar->update();
  connectUpdateSignal();
  mComposerScaleBar->endCommand();
}

void QgsComposerScaleBarWidget::on_mSegmentsLeftSpinBox_valueChanged( int i )
{
  if ( !mComposerScaleBar )
  {
    return;
  }

  mComposerScaleBar->beginCommand( tr( "Scalebar segments left" ), QgsComposerMergeCommand::ScaleBarSegmentsLeft );
  disconnectUpdateSignal();
  mComposerScaleBar->setNumSegmentsLeft( i );
  mComposerScaleBar->update();
  connectUpdateSignal();
  mComposerScaleBar->endCommand();
}

void QgsComposerScaleBarWidget::on_mNumberOfSegmentsSpinBox_valueChanged( int i )
{
  if ( !mComposerScaleBar )
  {
    return;
  }

  mComposerScaleBar->beginCommand( tr( "Number of scalebar segments changed" ), QgsComposerMergeCommand::ScaleBarNSegments );
  disconnectUpdateSignal();
  mComposerScaleBar->setNumSegments( i );
  mComposerScaleBar->update();
  connectUpdateSignal();
  mComposerScaleBar->endCommand();
}

void QgsComposerScaleBarWidget::on_mHeightSpinBox_valueChanged( double d )
{
  if ( !mComposerScaleBar )
  {
    return;
  }
  mComposerScaleBar->beginCommand( tr( "Scalebar height changed" ), QgsComposerMergeCommand::ScaleBarHeight );
  disconnectUpdateSignal();
  mComposerScaleBar->setHeight( d );
  mComposerScaleBar->update();
  connectUpdateSignal();
  mComposerScaleBar->endCommand();
}

void QgsComposerScaleBarWidget::on_mFontButton_clicked()
{
  if ( !mComposerScaleBar )
  {
    return;
  }

  bool dialogAccepted;
  QFont newFont = QgisGui::getFont( dialogAccepted, mComposerScaleBar->font() );
  if ( dialogAccepted )
  {
    mComposerScaleBar->beginCommand( tr( "Scalebar font changed" ) );
    disconnectUpdateSignal();
    mComposerScaleBar->setFont( newFont );
    connectUpdateSignal();
    mComposerScaleBar->endCommand();
  }
  mComposerScaleBar->update();
}

void QgsComposerScaleBarWidget::on_mFontColorButton_colorChanged( const QColor& newColor )
{
  if ( !mComposerScaleBar )
  {
    return;
  }

  mComposerScaleBar->beginCommand( tr( "Scalebar font color changed" ) );
  disconnectUpdateSignal();
  mComposerScaleBar->setFontColor( newColor );
  mComposerScaleBar->update();
  connectUpdateSignal();
  mComposerScaleBar->endCommand();
}

void QgsComposerScaleBarWidget::on_mFillColorButton_colorChanged( const QColor& newColor )
{
  if ( !mComposerScaleBar )
  {
    return;
  }

  mComposerScaleBar->beginCommand( tr( "Scalebar color changed" ) );
  disconnectUpdateSignal();
  QBrush newBrush = mComposerScaleBar->brush();
  newBrush.setColor( newColor );
  mComposerScaleBar->setBrush( newBrush );
  mComposerScaleBar->update();
  connectUpdateSignal();
  mComposerScaleBar->endCommand();
}

void QgsComposerScaleBarWidget::on_mFillColor2Button_colorChanged( const QColor &newColor )
{
  if ( !mComposerScaleBar )
  {
    return;
  }

  mComposerScaleBar->beginCommand( tr( "Scalebar secondary color changed" ) );
  disconnectUpdateSignal();
  QBrush newBrush = mComposerScaleBar->brush2();
  newBrush.setColor( newColor );
  mComposerScaleBar->setBrush2( newBrush );
  mComposerScaleBar->update();
  connectUpdateSignal();
  mComposerScaleBar->endCommand();
}

void QgsComposerScaleBarWidget::on_mStrokeColorButton_colorChanged( const QColor& newColor )
{
  if ( !mComposerScaleBar )
  {
    return;
  }

  mComposerScaleBar->beginCommand( tr( "Scalebar line color changed" ) );
  disconnectUpdateSignal();
  QPen newPen = mComposerScaleBar->pen();
  newPen.setColor( newColor );
  mComposerScaleBar->setPen( newPen );
  mComposerScaleBar->update();
  connectUpdateSignal();
  mComposerScaleBar->endCommand();
}

void QgsComposerScaleBarWidget::on_mUnitLabelLineEdit_textChanged( const QString& text )
{
  if ( !mComposerScaleBar )
  {
    return;
  }

  mComposerScaleBar->beginCommand( tr( "Scalebar unit text" ), QgsComposerMergeCommand::ScaleBarUnitText );
  disconnectUpdateSignal();
  mComposerScaleBar->setUnitLabeling( text );
  mComposerScaleBar->update();
  connectUpdateSignal();
  mComposerScaleBar->endCommand();
}

void QgsComposerScaleBarWidget::on_mMapUnitsPerBarUnitSpinBox_valueChanged( double d )
{
  if ( !mComposerScaleBar )
  {
    return;
  }

  mComposerScaleBar->beginCommand( tr( "Scalebar map units per segment" ), QgsComposerMergeCommand::ScaleBarMapUnitsSegment );
  disconnectUpdateSignal();
  mComposerScaleBar->setNumMapUnitsPerScaleBarUnit( d );
  mComposerScaleBar->update();
  connectUpdateSignal();
  mComposerScaleBar->endCommand();
}

void QgsComposerScaleBarWidget::on_mStyleComboBox_currentIndexChanged( const QString& text )
{
  if ( !mComposerScaleBar )
  {
    return;
  }

  mComposerScaleBar->beginCommand( tr( "Scalebar style changed" ) );
  disconnectUpdateSignal();
  QString untranslatedStyleName;
  if ( text == tr( "Single Box" ) )
  {
    untranslatedStyleName = "Single Box";
  }
  else if ( text == tr( "Double Box" ) )
  {
    untranslatedStyleName = "Double Box";
  }
  else if ( text == tr( "Line Ticks Middle" ) )
  {
    untranslatedStyleName = "Line Ticks Middle";
  }
  else if ( text == tr( "Line Ticks Middle" ) )
  {
    untranslatedStyleName = "Line Ticks Middle";
  }
  else if ( text == tr( "Line Ticks Down" ) )
  {
    untranslatedStyleName = "Line Ticks Down";
  }
  else if ( text == tr( "Line Ticks Up" ) )
  {
    untranslatedStyleName = "Line Ticks Up";
  }
  else if ( text == tr( "Numeric" ) )
  {
    untranslatedStyleName = "Numeric";
  }

  //disable or enable controls which apply to specific scale bar styles
  toggleStyleSpecificControls( untranslatedStyleName );

  mComposerScaleBar->setStyle( untranslatedStyleName );
  mComposerScaleBar->update();
  connectUpdateSignal();
  mComposerScaleBar->endCommand();
}

void QgsComposerScaleBarWidget::toggleStyleSpecificControls( const QString& style )
{
  if ( style == "Numeric" )
  {
    //Disable controls which don't apply to numeric scale bars
    mGroupBoxUnits->setEnabled( false );
    mGroupBoxUnits->setCollapsed( true );
    mGroupBoxSegments->setEnabled( false );
    mGroupBoxSegments->setCollapsed( true );
    mLabelBarSpaceSpinBox->setEnabled( false );
    mLineWidthSpinBox->setEnabled( false );
    mFillColorButton->setEnabled( false );
    mFillColor2Button->setEnabled( false );
    mStrokeColorButton->setEnabled( false );
    mLineJoinStyleCombo->setEnabled( false );
    mLineCapStyleCombo->setEnabled( false );
  }
  else
  {
    //Enable controls
    mGroupBoxUnits->setEnabled( true );
    mGroupBoxSegments->setEnabled( true );
    mLabelBarSpaceSpinBox->setEnabled( true );
    mLineWidthSpinBox->setEnabled( true );
    mFillColorButton->setEnabled( true );
    mFillColor2Button->setEnabled( true );
    mStrokeColorButton->setEnabled( true );
    if ( style == "Single Box" || style == "Double Box" )
    {
      mLineJoinStyleCombo->setEnabled( true );
      mLineCapStyleCombo->setEnabled( false );
    }
    else
    {
      mLineJoinStyleCombo->setEnabled( false );
      mLineCapStyleCombo->setEnabled( true );
    }

  }
}

void QgsComposerScaleBarWidget::on_mLabelBarSpaceSpinBox_valueChanged( double d )
{
  if ( !mComposerScaleBar )
  {
    return;
  }

  mComposerScaleBar->beginCommand( tr( "Scalebar label bar space" ), QgsComposerMergeCommand::ScaleBarLabelBarSize );
  disconnectUpdateSignal();
  mComposerScaleBar->setLabelBarSpace( d );
  mComposerScaleBar->update();
  connectUpdateSignal();
  mComposerScaleBar->endCommand();
}

void QgsComposerScaleBarWidget::on_mBoxSizeSpinBox_valueChanged( double d )
{
  if ( !mComposerScaleBar )
  {
    return;
  }

  mComposerScaleBar->beginCommand( tr( "Scalebar box content space" ), QgsComposerMergeCommand::ScaleBarBoxContentSpace );
  disconnectUpdateSignal();
  mComposerScaleBar->setBoxContentSpace( d );
  mComposerScaleBar->update();
  connectUpdateSignal();
  mComposerScaleBar->endCommand();
}

void QgsComposerScaleBarWidget::on_mAlignmentComboBox_currentIndexChanged( int index )
{
  if ( !mComposerScaleBar )
  {
    return;
  }

  mComposerScaleBar->beginCommand( tr( "Scalebar alignment" ) );
  disconnectUpdateSignal();
  mComposerScaleBar->setAlignment(( QgsComposerScaleBar::Alignment ) index );
  connectUpdateSignal();
  mComposerScaleBar->endCommand();
}

void QgsComposerScaleBarWidget::on_mUnitsComboBox_currentIndexChanged( int index )
{
  if ( !mComposerScaleBar )
  {
    return;
  }

  QVariant unitData = mUnitsComboBox->itemData( index );
  if ( unitData.type() == QVariant::Invalid )
  {
    return;
  }

  disconnectUpdateSignal();
  mComposerScaleBar->setUnits(( QgsComposerScaleBar::ScaleBarUnits )unitData.toInt() );
  switch ( mUnitsComboBox->currentIndex() )
  {
    case 0:
    {
      mComposerScaleBar->beginCommand( tr( "Scalebar changed to map units" ) );
      mComposerScaleBar->applyDefaultSize( QgsComposerScaleBar::MapUnits );
      break;
    }
    case 2:
    {
      mComposerScaleBar->beginCommand( tr( "Scalebar changed to feet" ) );
      mComposerScaleBar->applyDefaultSize( QgsComposerScaleBar::Feet );
      break;
    }
    case 3:
    {
      mComposerScaleBar->beginCommand( tr( "Scalebar changed to nautical miles" ) );
      mComposerScaleBar->applyDefaultSize( QgsComposerScaleBar::NauticalMiles );
      break;
    }
    case 1:
    default:
    {
      mComposerScaleBar->beginCommand( tr( "Scalebar changed to meters" ) );
      mComposerScaleBar->applyDefaultSize( QgsComposerScaleBar::Meters );
      break;
    }
  }

  mComposerScaleBar->update();

  mUnitLabelLineEdit->setText( mComposerScaleBar->unitLabeling() );
  mSegmentSizeSpinBox->setValue( mComposerScaleBar->numUnitsPerSegment() );
  mMapUnitsPerBarUnitSpinBox->setValue( mComposerScaleBar->numMapUnitsPerScaleBarUnit() );

  connectUpdateSignal();
  mComposerScaleBar->endCommand();
}

void QgsComposerScaleBarWidget::blockMemberSignals( bool block )
{
  mSegmentSizeSpinBox->blockSignals( block );
  mNumberOfSegmentsSpinBox->blockSignals( block );
  mSegmentsLeftSpinBox->blockSignals( block );
  mStyleComboBox->blockSignals( block );
  mUnitLabelLineEdit->blockSignals( block );
  mMapUnitsPerBarUnitSpinBox->blockSignals( block );
  mHeightSpinBox->blockSignals( block );
  mLineWidthSpinBox->blockSignals( block );
  mLabelBarSpaceSpinBox->blockSignals( block );
  mBoxSizeSpinBox->blockSignals( block );
  mAlignmentComboBox->blockSignals( block );
  mUnitsComboBox->blockSignals( block );
  mLineJoinStyleCombo->blockSignals( block );
  mLineCapStyleCombo->blockSignals( block );
  mFontColorButton->blockSignals( block );
  mFillColorButton->blockSignals( block );
  mFillColor2Button->blockSignals( block );
  mStrokeColorButton->blockSignals( block );
  mSegmentSizeRadioGroup.blockSignals( block );
  mMapItemComboBox->blockSignals( block );
}

void QgsComposerScaleBarWidget::connectUpdateSignal()
{
  if ( mComposerScaleBar )
  {
    QObject::connect( mComposerScaleBar, SIGNAL( itemChanged() ), this, SLOT( setGuiElements() ) );
  }
}

void QgsComposerScaleBarWidget::disconnectUpdateSignal()
{
  if ( mComposerScaleBar )
  {
    QObject::disconnect( mComposerScaleBar, SIGNAL( itemChanged() ), this, SLOT( setGuiElements() ) );
  }
}

void QgsComposerScaleBarWidget::on_mLineJoinStyleCombo_currentIndexChanged( int index )
{
  Q_UNUSED( index );
  if ( !mComposerScaleBar )
  {
    return;
  }

  mComposerScaleBar->beginCommand( tr( "Scalebar line join style" ) );
  mComposerScaleBar->setLineJoinStyle( mLineJoinStyleCombo->penJoinStyle() );
  mComposerScaleBar->endCommand();
}

void QgsComposerScaleBarWidget::on_mLineCapStyleCombo_currentIndexChanged( int index )
{
  Q_UNUSED( index );
  if ( !mComposerScaleBar )
  {
    return;
  }

  mComposerScaleBar->beginCommand( tr( "Scalebar line cap style" ) );
  mComposerScaleBar->setLineCapStyle( mLineCapStyleCombo->penCapStyle() );
  mComposerScaleBar->endCommand();
}

void QgsComposerScaleBarWidget::segmentSizeRadioChanged( QAbstractButton* radio )
{
  bool fixedSizeMode = radio == mFixedSizeRadio;
  mMinWidthSpinBox->setEnabled( !fixedSizeMode );
  mMaxWidthSpinBox->setEnabled( !fixedSizeMode );
  mSegmentSizeSpinBox->setEnabled( fixedSizeMode );

  if ( !mComposerScaleBar )
  {
    return;
  }

  mComposerScaleBar->beginCommand( tr( "Scalebar segment size mode" ), QgsComposerMergeCommand::ScaleBarSegmentSize );
  disconnectUpdateSignal();
  if ( mFixedSizeRadio->isChecked() )
  {
    mComposerScaleBar->setSegmentSizeMode( QgsComposerScaleBar::SegmentSizeFixed );
    mComposerScaleBar->setNumUnitsPerSegment( mSegmentSizeSpinBox->value() );
  }
  else /*if(mFitWidthRadio->isChecked())*/
  {
    mComposerScaleBar->setSegmentSizeMode( QgsComposerScaleBar::SegmentSizeFitWidth );
  }
  mComposerScaleBar->update();
  connectUpdateSignal();
  mComposerScaleBar->endCommand();
}

void QgsComposerScaleBarWidget::composerMapChanged( QgsComposerItem* item )
{
  QgsComposerMap* composerMap = dynamic_cast< QgsComposerMap* >( item );
  if ( !composerMap )
  {
    return;
  }

  //set it to scale bar
  mComposerScaleBar->beginCommand( tr( "Scalebar map changed" ) );
  disconnectUpdateSignal();
  mComposerScaleBar->setComposerMap( composerMap );
  mComposerScaleBar->update();
  connectUpdateSignal();
  mComposerScaleBar->endCommand();
}

void QgsComposerScaleBarWidget::on_mMinWidthSpinBox_valueChanged( double )
{
  if ( !mComposerScaleBar )
  {
    return;
  }

  mComposerScaleBar->beginCommand( tr( "Scalebar segment size mode" ), QgsComposerMergeCommand::ScaleBarSegmentSize );
  disconnectUpdateSignal();
  mComposerScaleBar->setMinBarWidth( mMinWidthSpinBox->value() );
  mComposerScaleBar->update();
  connectUpdateSignal();
  mComposerScaleBar->endCommand();
}

void QgsComposerScaleBarWidget::on_mMaxWidthSpinBox_valueChanged( double )
{
  if ( !mComposerScaleBar )
  {
    return;
  }

  mComposerScaleBar->beginCommand( tr( "Scalebar segment size mode" ), QgsComposerMergeCommand::ScaleBarSegmentSize );
  disconnectUpdateSignal();
  mComposerScaleBar->setMaxBarWidth( mMaxWidthSpinBox->value() );
  mComposerScaleBar->update();
  connectUpdateSignal();
  mComposerScaleBar->endCommand();
}
