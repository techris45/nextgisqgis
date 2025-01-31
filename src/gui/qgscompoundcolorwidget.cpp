/***************************************************************************
    qgscompoundcolorwidget.cpp
    --------------------------
    begin                : April 2016
    copyright            : (C) 2016 by Nyall Dawson
    email                : nyall dot dawson at gmail dot com
 ***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include "qgscompoundcolorwidget.h"
#include "qgscolorscheme.h"
#include "qgscolorschemeregistry.h"
#include "qgssymbollayerv2utils.h"
#include "qgscursors.h"
#include "qgsapplication.h"
#include <QSettings>
#include <QPushButton>
#include <QMenu>
#include <QToolButton>
#include <QFileDialog>
#include <QMessageBox>
#include <QDesktopWidget>
#include <QMouseEvent>
#include <QInputDialog>
#include <QVBoxLayout>

QgsCompoundColorWidget::QgsCompoundColorWidget( QWidget *parent, const QColor& color, Layout widgetLayout )
    : QgsPanelWidget( parent )
    , mAllowAlpha( true )
    , mLastCustomColorIndex( 0 )
    , mPickingColor( false )
{
  setupUi( this );

  if ( widgetLayout == LayoutVertical )
  {
    // shuffle stuff around
    QVBoxLayout* newLayout = new QVBoxLayout();
    newLayout->setMargin( 0 );
    newLayout->setContentsMargins( 0, 0, 0, 0 );
    newLayout->addWidget( mTabWidget );
    newLayout->addWidget( mSlidersWidget );
    newLayout->addWidget( mPreviewWidget );
    newLayout->addWidget( mSwatchesWidget );
    delete layout();
    setLayout( newLayout );
  }

  QSettings settings;

  mSchemeList->header()->hide();
  mSchemeList->setColumnWidth( 0, 44 );

  //get schemes with ShowInColorDialog set
  refreshSchemeComboBox();
  QList<QgsColorScheme *> schemeList = QgsColorSchemeRegistry::instance()->schemes( QgsColorScheme::ShowInColorDialog );

  //choose a reasonable starting scheme
  int activeScheme = settings.value( "/Windows/ColorDialog/activeScheme", 0 ).toInt();
  activeScheme = activeScheme >= mSchemeComboBox->count() ? 0 : activeScheme;

  mSchemeList->setScheme( schemeList.at( activeScheme ) );

  mSchemeComboBox->setCurrentIndex( activeScheme );
  updateActionsForCurrentScheme();

  //listen out for selection changes in list, so we can enable/disable the copy colors option
  connect( mSchemeList->selectionModel(), SIGNAL( selectionChanged( QItemSelection, QItemSelection ) ), this, SLOT( listSelectionChanged( QItemSelection, QItemSelection ) ) );
  //copy action defaults to disabled
  mActionCopyColors->setEnabled( false );

  connect( mActionCopyColors, SIGNAL( triggered() ), mSchemeList, SLOT( copyColors() ) );
  connect( mActionPasteColors, SIGNAL( triggered() ), mSchemeList, SLOT( pasteColors() ) );
  connect( mActionExportColors, SIGNAL( triggered() ), this, SLOT( exportColors() ) );
  connect( mActionImportColors, SIGNAL( triggered() ), this, SLOT( importColors() ) );
  connect( mActionImportPalette, SIGNAL( triggered() ), this, SLOT( importPalette() ) );
  connect( mActionRemovePalette, SIGNAL( triggered() ), this, SLOT( removePalette() ) );
  connect( mActionNewPalette, SIGNAL( triggered() ), this, SLOT( newPalette() ) );
  connect( mRemoveColorsFromSchemeButton, SIGNAL( clicked() ), mSchemeList, SLOT( removeSelection() ) );

  QMenu* schemeMenu = new QMenu( mSchemeToolButton );
  schemeMenu->addAction( mActionCopyColors );
  schemeMenu->addAction( mActionPasteColors );
  schemeMenu->addSeparator();
  schemeMenu->addAction( mActionImportColors );
  schemeMenu->addAction( mActionExportColors );
  schemeMenu->addSeparator();
  schemeMenu->addAction( mActionNewPalette );
  schemeMenu->addAction( mActionImportPalette );
  schemeMenu->addAction( mActionRemovePalette );
  schemeMenu->addAction( mActionShowInButtons );
  mSchemeToolButton->setMenu( schemeMenu );

  connect( mSchemeComboBox, SIGNAL( currentIndexChanged( int ) ), this, SLOT( schemeIndexChanged( int ) ) );
  connect( mSchemeList, SIGNAL( colorSelected( QColor ) ), this, SLOT( setColor( QColor ) ) );

  mOldColorLabel->hide();

  mVerticalRamp->setOrientation( QgsColorRampWidget::Vertical );
  mVerticalRamp->setInteriorMargin( 2 );
  mVerticalRamp->setShowFrame( true );

  mRedSlider->setComponent( QgsColorWidget::Red );
  mGreenSlider->setComponent( QgsColorWidget::Green );
  mBlueSlider->setComponent( QgsColorWidget::Blue );
  mHueSlider->setComponent( QgsColorWidget::Hue );
  mSaturationSlider->setComponent( QgsColorWidget::Saturation );
  mValueSlider->setComponent( QgsColorWidget::Value );
  mAlphaSlider->setComponent( QgsColorWidget::Alpha );

  mSwatchButton1->setShowMenu( false );
  mSwatchButton1->setBehaviour( QgsColorButtonV2::SignalOnly );
  mSwatchButton2->setShowMenu( false );
  mSwatchButton2->setBehaviour( QgsColorButtonV2::SignalOnly );
  mSwatchButton3->setShowMenu( false );
  mSwatchButton3->setBehaviour( QgsColorButtonV2::SignalOnly );
  mSwatchButton4->setShowMenu( false );
  mSwatchButton4->setBehaviour( QgsColorButtonV2::SignalOnly );
  mSwatchButton5->setShowMenu( false );
  mSwatchButton5->setBehaviour( QgsColorButtonV2::SignalOnly );
  mSwatchButton6->setShowMenu( false );
  mSwatchButton6->setBehaviour( QgsColorButtonV2::SignalOnly );
  mSwatchButton7->setShowMenu( false );
  mSwatchButton7->setBehaviour( QgsColorButtonV2::SignalOnly );
  mSwatchButton8->setShowMenu( false );
  mSwatchButton8->setBehaviour( QgsColorButtonV2::SignalOnly );
  mSwatchButton9->setShowMenu( false );
  mSwatchButton9->setBehaviour( QgsColorButtonV2::SignalOnly );
  mSwatchButton10->setShowMenu( false );
  mSwatchButton10->setBehaviour( QgsColorButtonV2::SignalOnly );
  mSwatchButton11->setShowMenu( false );
  mSwatchButton11->setBehaviour( QgsColorButtonV2::SignalOnly );
  mSwatchButton12->setShowMenu( false );
  mSwatchButton12->setBehaviour( QgsColorButtonV2::SignalOnly );
  mSwatchButton13->setShowMenu( false );
  mSwatchButton13->setBehaviour( QgsColorButtonV2::SignalOnly );
  mSwatchButton14->setShowMenu( false );
  mSwatchButton14->setBehaviour( QgsColorButtonV2::SignalOnly );
  mSwatchButton15->setShowMenu( false );
  mSwatchButton15->setBehaviour( QgsColorButtonV2::SignalOnly );
  mSwatchButton16->setShowMenu( false );
  mSwatchButton16->setBehaviour( QgsColorButtonV2::SignalOnly );
  //restore custom colors
  mSwatchButton1->setColor( settings.value( "/Windows/ColorDialog/customColor1", QVariant( QColor() ) ).value<QColor>() );
  mSwatchButton2->setColor( settings.value( "/Windows/ColorDialog/customColor2", QVariant( QColor() ) ).value<QColor>() );
  mSwatchButton3->setColor( settings.value( "/Windows/ColorDialog/customColor3", QVariant( QColor() ) ).value<QColor>() );
  mSwatchButton4->setColor( settings.value( "/Windows/ColorDialog/customColor4", QVariant( QColor() ) ).value<QColor>() );
  mSwatchButton5->setColor( settings.value( "/Windows/ColorDialog/customColor5", QVariant( QColor() ) ).value<QColor>() );
  mSwatchButton6->setColor( settings.value( "/Windows/ColorDialog/customColor6", QVariant( QColor() ) ).value<QColor>() );
  mSwatchButton7->setColor( settings.value( "/Windows/ColorDialog/customColor7", QVariant( QColor() ) ).value<QColor>() );
  mSwatchButton8->setColor( settings.value( "/Windows/ColorDialog/customColor8", QVariant( QColor() ) ).value<QColor>() );
  mSwatchButton9->setColor( settings.value( "/Windows/ColorDialog/customColor9", QVariant( QColor() ) ).value<QColor>() );
  mSwatchButton10->setColor( settings.value( "/Windows/ColorDialog/customColor10", QVariant( QColor() ) ).value<QColor>() );
  mSwatchButton11->setColor( settings.value( "/Windows/ColorDialog/customColor11", QVariant( QColor() ) ).value<QColor>() );
  mSwatchButton12->setColor( settings.value( "/Windows/ColorDialog/customColor12", QVariant( QColor() ) ).value<QColor>() );
  mSwatchButton13->setColor( settings.value( "/Windows/ColorDialog/customColor13", QVariant( QColor() ) ).value<QColor>() );
  mSwatchButton14->setColor( settings.value( "/Windows/ColorDialog/customColor14", QVariant( QColor() ) ).value<QColor>() );
  mSwatchButton15->setColor( settings.value( "/Windows/ColorDialog/customColor15", QVariant( QColor() ) ).value<QColor>() );
  mSwatchButton16->setColor( settings.value( "/Windows/ColorDialog/customColor16", QVariant( QColor() ) ).value<QColor>() );

  //restore sample radius
  mSpinBoxRadius->setValue( settings.value( "/Windows/ColorDialog/sampleRadius", 1 ).toInt() );
  mSamplePreview->setColor( QColor() );

  if ( color.isValid() )
  {
    setColor( color );
  }

  //restore active component radio button
  int activeRadio = settings.value( "/Windows/ColorDialog/activeComponent", 2 ).toInt();
  switch ( activeRadio )
  {
    case 0:
      mHueRadio->setChecked( true );
      break;
    case 1:
      mSaturationRadio->setChecked( true );
      break;
    case 2:
      mValueRadio->setChecked( true );
      break;
    case 3:
      mRedRadio->setChecked( true );
      break;
    case 4:
      mGreenRadio->setChecked( true );
      break;
    case 5:
      mBlueRadio->setChecked( true );
      break;
  }
  int currentTab = settings.value( "/Windows/ColorDialog/activeTab", 0 ).toInt();
  mTabWidget->setCurrentIndex( currentTab );

#ifdef Q_OS_MAC
  //disable color picker tab for OSX, as it is impossible to grab the mouse under OSX
  //see note for QWidget::grabMouse() re OSX Cocoa
  //http://qt-project.org/doc/qt-4.8/qwidget.html#grabMouse
  mTabWidget->removeTab( 3 );
#endif

  //setup connections
  connect( mColorBox, SIGNAL( colorChanged( QColor ) ), this, SLOT( setColor( QColor ) ) );
  connect( mColorWheel, SIGNAL( colorChanged( QColor ) ), this, SLOT( setColor( QColor ) ) );
  connect( mColorText, SIGNAL( colorChanged( QColor ) ), this, SLOT( setColor( QColor ) ) );
  connect( mVerticalRamp, SIGNAL( colorChanged( QColor ) ), this, SLOT( setColor( QColor ) ) );
  connect( mRedSlider, SIGNAL( colorChanged( QColor ) ), this, SLOT( setColor( QColor ) ) );
  connect( mGreenSlider, SIGNAL( colorChanged( QColor ) ), this, SLOT( setColor( QColor ) ) );
  connect( mBlueSlider, SIGNAL( colorChanged( QColor ) ), this, SLOT( setColor( QColor ) ) );
  connect( mHueSlider, SIGNAL( colorChanged( QColor ) ), this, SLOT( setColor( QColor ) ) );
  connect( mValueSlider, SIGNAL( colorChanged( QColor ) ), this, SLOT( setColor( QColor ) ) );
  connect( mSaturationSlider, SIGNAL( colorChanged( QColor ) ), this, SLOT( setColor( QColor ) ) );
  connect( mAlphaSlider, SIGNAL( colorChanged( QColor ) ), this, SLOT( setColor( QColor ) ) );
  connect( mColorPreview, SIGNAL( colorChanged( QColor ) ), this, SLOT( setColor( QColor ) ) );
  connect( mSwatchButton1, SIGNAL( colorClicked( QColor ) ), this, SLOT( setColor( QColor ) ) );
  connect( mSwatchButton2, SIGNAL( colorClicked( QColor ) ), this, SLOT( setColor( QColor ) ) );
  connect( mSwatchButton3, SIGNAL( colorClicked( QColor ) ), this, SLOT( setColor( QColor ) ) );
  connect( mSwatchButton4, SIGNAL( colorClicked( QColor ) ), this, SLOT( setColor( QColor ) ) );
  connect( mSwatchButton5, SIGNAL( colorClicked( QColor ) ), this, SLOT( setColor( QColor ) ) );
  connect( mSwatchButton6, SIGNAL( colorClicked( QColor ) ), this, SLOT( setColor( QColor ) ) );
  connect( mSwatchButton7, SIGNAL( colorClicked( QColor ) ), this, SLOT( setColor( QColor ) ) );
  connect( mSwatchButton8, SIGNAL( colorClicked( QColor ) ), this, SLOT( setColor( QColor ) ) );
  connect( mSwatchButton9, SIGNAL( colorClicked( QColor ) ), this, SLOT( setColor( QColor ) ) );
  connect( mSwatchButton10, SIGNAL( colorClicked( QColor ) ), this, SLOT( setColor( QColor ) ) );
  connect( mSwatchButton11, SIGNAL( colorClicked( QColor ) ), this, SLOT( setColor( QColor ) ) );
  connect( mSwatchButton12, SIGNAL( colorClicked( QColor ) ), this, SLOT( setColor( QColor ) ) );
  connect( mSwatchButton13, SIGNAL( colorClicked( QColor ) ), this, SLOT( setColor( QColor ) ) );
  connect( mSwatchButton14, SIGNAL( colorClicked( QColor ) ), this, SLOT( setColor( QColor ) ) );
  connect( mSwatchButton15, SIGNAL( colorClicked( QColor ) ), this, SLOT( setColor( QColor ) ) );
  connect( mSwatchButton16, SIGNAL( colorClicked( QColor ) ), this, SLOT( setColor( QColor ) ) );
}

QgsCompoundColorWidget::~QgsCompoundColorWidget()
{
  saveSettings();
}

QColor QgsCompoundColorWidget::color() const
{
  //all widgets should have the same color, so it shouldn't matter
  //which we fetch it from
  return mColorPreview->color();
}

void QgsCompoundColorWidget::setAllowAlpha( const bool allowAlpha )
{
  mAllowAlpha = allowAlpha;
  mAlphaLabel->setVisible( allowAlpha );
  mAlphaSlider->setVisible( allowAlpha );
  if ( !allowAlpha )
  {
    mAlphaLayout->setContentsMargins( 0, 0, 0, 0 );
    mAlphaLayout->setSpacing( 0 );
  }
}

void QgsCompoundColorWidget::importColors()
{
  QSettings s;
  QString lastDir = s.value( "/UI/lastGplPaletteDir", QDir::homePath() ).toString();
  QString filePath = QFileDialog::getOpenFileName( this, tr( "Select palette file" ), lastDir, "GPL (*.gpl);;All files (*.*)" );
  activateWindow();
  if ( filePath.isEmpty() )
  {
    return;
  }

  //check if file exists
  QFileInfo fileInfo( filePath );
  if ( !fileInfo.exists() || !fileInfo.isReadable() )
  {
    QMessageBox::critical( nullptr, tr( "Invalid file" ), tr( "Error, file does not exist or is not readable" ) );
    return;
  }

  s.setValue( "/UI/lastGplPaletteDir", fileInfo.absolutePath() );
  QFile file( filePath );
  bool importOk = mSchemeList->importColorsFromGpl( file );
  if ( !importOk )
  {
    QMessageBox::critical( nullptr, tr( "Invalid file" ), tr( "Error, no colors found in palette file" ) );
    return;
  }
}

void QgsCompoundColorWidget::refreshSchemeComboBox()
{
  mSchemeComboBox->blockSignals( true );
  mSchemeComboBox->clear();
  QList<QgsColorScheme *> schemeList = QgsColorSchemeRegistry::instance()->schemes( QgsColorScheme::ShowInColorDialog );
  QList<QgsColorScheme *>::const_iterator schemeIt = schemeList.constBegin();
  for ( ; schemeIt != schemeList.constEnd(); ++schemeIt )
  {
    mSchemeComboBox->addItem(( *schemeIt )->schemeName() );
  }
  mSchemeComboBox->blockSignals( false );
}

void QgsCompoundColorWidget::importPalette()
{
  QSettings s;
  QString lastDir = s.value( "/UI/lastGplPaletteDir", QDir::homePath() ).toString();
  QString filePath = QFileDialog::getOpenFileName( this, tr( "Select palette file" ), lastDir, "GPL (*.gpl);;All files (*.*)" );
  activateWindow();
  if ( filePath.isEmpty() )
  {
    return;
  }

  //check if file exists
  QFileInfo fileInfo( filePath );
  if ( !fileInfo.exists() || !fileInfo.isReadable() )
  {
    QMessageBox::critical( nullptr, tr( "Invalid file" ), tr( "Error, file does not exist or is not readable" ) );
    return;
  }

  s.setValue( "/UI/lastGplPaletteDir", fileInfo.absolutePath() );
  QFile file( filePath );

  QgsNamedColorList importedColors;
  bool ok = false;
  QString paletteName;
  importedColors = QgsSymbolLayerV2Utils::importColorsFromGpl( file, ok, paletteName );
  if ( !ok )
  {
    QMessageBox::critical( nullptr, tr( "Invalid file" ), tr( "Palette file is not readable" ) );
    return;
  }

  if ( importedColors.length() == 0 )
  {
    //no imported colors
    QMessageBox::critical( nullptr, tr( "Invalid file" ), tr( "No colors found in palette file" ) );
    return;
  }

  //TODO - handle conflicting file names, name for new palette
  QgsUserColorScheme* importedScheme = new QgsUserColorScheme( fileInfo.fileName() );
  importedScheme->setName( paletteName );
  importedScheme->setColors( importedColors );

  QgsColorSchemeRegistry::instance()->addColorScheme( importedScheme );

  //refresh combobox
  refreshSchemeComboBox();
  mSchemeComboBox->setCurrentIndex( mSchemeComboBox->count() - 1 );
}

void QgsCompoundColorWidget::removePalette()
{
  //get current scheme
  QList<QgsColorScheme *> schemeList = QgsColorSchemeRegistry::instance()->schemes( QgsColorScheme::ShowInColorDialog );
  int prevIndex = mSchemeComboBox->currentIndex();
  if ( prevIndex >= schemeList.length() )
  {
    return;
  }

  //make user scheme is a user removable scheme
  QgsUserColorScheme* userScheme = dynamic_cast<QgsUserColorScheme*>( schemeList.at( prevIndex ) );
  if ( !userScheme )
  {
    return;
  }

  if ( QMessageBox::question( this, tr( "Remove Color Palette" ),
                              QString( tr( "Are you sure you want to remove %1?" ) ).arg( userScheme->schemeName() ),
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No ) != QMessageBox::Yes )
  {
    //user cancelled
    return;
  }

  //remove palette and associated gpl file
  if ( !userScheme->erase() )
  {
    //something went wrong
    return;
  }

  //remove scheme from registry
  QgsColorSchemeRegistry::instance()->removeColorScheme( userScheme );
  refreshSchemeComboBox();
  prevIndex = qMax( qMin( prevIndex, mSchemeComboBox->count() - 1 ), 0 );
  mSchemeComboBox->setCurrentIndex( prevIndex );
}

void QgsCompoundColorWidget::newPalette()
{
  bool ok = false;
  QString name = QInputDialog::getText( this, tr( "Create New Palette" ), tr( "Enter a name for the new palette:" ),
                                        QLineEdit::Normal, tr( "New palette" ), &ok );

  if ( !ok || name.isEmpty() )
  {
    //user cancelled
    return;
  }

  //generate file name for new palette
  QDir palettePath( gplFilePath() );
  QRegExp badChars( "[,^@={}\\[\\]~!?:&*\"|#%<>$\"'();`' /\\\\]" );
  QString filename = name.simplified().toLower().replace( badChars, QLatin1String( "_" ) );
  if ( filename.isEmpty() )
  {
    filename = tr( "new_palette" );
  }
  QFileInfo destFileInfo( palettePath.filePath( filename + ".gpl" ) );
  int fileNumber = 1;
  while ( destFileInfo.exists() )
  {
    //try to generate a unique file name
    destFileInfo = QFileInfo( palettePath.filePath( filename + QString( "%1.gpl" ).arg( fileNumber ) ) );
    fileNumber++;
  }

  QgsUserColorScheme* newScheme = new QgsUserColorScheme( destFileInfo.fileName() );
  newScheme->setName( name );

  QgsColorSchemeRegistry::instance()->addColorScheme( newScheme );

  //refresh combobox and set new scheme as active
  refreshSchemeComboBox();
  mSchemeComboBox->setCurrentIndex( mSchemeComboBox->count() - 1 );
}

QString QgsCompoundColorWidget::gplFilePath()
{
  QString palettesDir = QgsApplication::qgisSettingsDirPath() + "/palettes";

  QDir localDir;
  if ( !localDir.mkpath( palettesDir ) )
  {
    return QString();
  }

  return palettesDir;
}

void QgsCompoundColorWidget::exportColors()
{
  QSettings s;
  QString lastDir = s.value( "/UI/lastGplPaletteDir", QDir::homePath() ).toString();
  QString fileName = QFileDialog::getSaveFileName( this, tr( "Palette file" ), lastDir, "GPL (*.gpl)" );
  activateWindow();
  if ( fileName.isEmpty() )
  {
    return;
  }

  // ensure filename contains extension
  if ( !fileName.endsWith( ".gpl", Qt::CaseInsensitive ) )
  {
    fileName += ".gpl";
  }

  QFileInfo fileInfo( fileName );
  s.setValue( "/UI/lastGplPaletteDir", fileInfo.absolutePath() );

  QFile file( fileName );
  bool exportOk = mSchemeList->exportColorsToGpl( file );
  if ( !exportOk )
  {
    QMessageBox::critical( nullptr, tr( "Error exporting" ), tr( "Error writing palette file" ) );
    return;
  }
}

void QgsCompoundColorWidget::schemeIndexChanged( int index )
{
  //save changes to scheme
  if ( mSchemeList->isDirty() )
  {
    mSchemeList->saveColorsToScheme();
  }

  //get schemes with ShowInColorDialog set
  QList<QgsColorScheme *> schemeList = QgsColorSchemeRegistry::instance()->schemes( QgsColorScheme::ShowInColorDialog );
  if ( index >= schemeList.length() )
  {
    return;
  }

  QgsColorScheme* scheme = schemeList.at( index );
  mSchemeList->setScheme( scheme );

  updateActionsForCurrentScheme();

  //copy action defaults to disabled
  mActionCopyColors->setEnabled( false );
}

void QgsCompoundColorWidget::listSelectionChanged( const QItemSelection &selected, const QItemSelection &deselected )
{
  Q_UNUSED( deselected );
  mActionCopyColors->setEnabled( selected.length() > 0 );
}

void QgsCompoundColorWidget::on_mAddCustomColorButton_clicked()
{
  switch ( mLastCustomColorIndex )
  {
    case 0:
      mSwatchButton1->setColor( mColorPreview->color() );
      break;
    case 1:
      mSwatchButton2->setColor( mColorPreview->color() );
      break;
    case 2:
      mSwatchButton3->setColor( mColorPreview->color() );
      break;
    case 3:
      mSwatchButton4->setColor( mColorPreview->color() );
      break;
    case 4:
      mSwatchButton5->setColor( mColorPreview->color() );
      break;
    case 5:
      mSwatchButton6->setColor( mColorPreview->color() );
      break;
    case 6:
      mSwatchButton7->setColor( mColorPreview->color() );
      break;
    case 7:
      mSwatchButton8->setColor( mColorPreview->color() );
      break;
    case 8:
      mSwatchButton9->setColor( mColorPreview->color() );
      break;
    case 9:
      mSwatchButton10->setColor( mColorPreview->color() );
      break;
    case 10:
      mSwatchButton11->setColor( mColorPreview->color() );
      break;
    case 11:
      mSwatchButton12->setColor( mColorPreview->color() );
      break;
    case 12:
      mSwatchButton13->setColor( mColorPreview->color() );
      break;
    case 13:
      mSwatchButton14->setColor( mColorPreview->color() );
      break;
    case 14:
      mSwatchButton15->setColor( mColorPreview->color() );
      break;
    case 15:
      mSwatchButton16->setColor( mColorPreview->color() );
      break;
  }
  mLastCustomColorIndex++;
  if ( mLastCustomColorIndex >= 16 )
  {
    mLastCustomColorIndex = 0;
  }
}

void QgsCompoundColorWidget::on_mSampleButton_clicked()
{
  //activate picker color
  QPixmap samplerPixmap = QPixmap(( const char ** ) sampler_cursor );
  setCursor( QCursor( samplerPixmap, 0, 0 ) );
  grabMouse();
  grabKeyboard();
  mPickingColor = true;
  setMouseTracking( true );
}

void QgsCompoundColorWidget::on_mTabWidget_currentChanged( int index )
{
  //disable radio buttons if not using the first tab, as they have no meaning for other tabs
  bool enabled = index == 0;
  mRedRadio->setEnabled( enabled );
  mBlueRadio->setEnabled( enabled );
  mGreenRadio->setEnabled( enabled );
  mHueRadio->setEnabled( enabled );
  mSaturationRadio->setEnabled( enabled );
  mValueRadio->setEnabled( enabled );
}

void QgsCompoundColorWidget::on_mActionShowInButtons_toggled( bool state )
{
  QgsUserColorScheme* scheme = dynamic_cast< QgsUserColorScheme* >( mSchemeList->scheme() );
  if ( scheme )
  {
    scheme->setShowSchemeInMenu( state );
  }
}

void QgsCompoundColorWidget::saveSettings()
{
  //save changes to scheme
  if ( mSchemeList->isDirty() )
  {
    mSchemeList->saveColorsToScheme();
  }

  QSettings settings;

  //record active component
  int activeRadio = 0;
  if ( mHueRadio->isChecked() )
    activeRadio = 0;
  if ( mSaturationRadio->isChecked() )
    activeRadio = 1;
  if ( mValueRadio->isChecked() )
    activeRadio = 2;
  if ( mRedRadio->isChecked() )
    activeRadio = 3;
  if ( mGreenRadio->isChecked() )
    activeRadio = 4;
  if ( mBlueRadio->isChecked() )
    activeRadio = 5;
  settings.setValue( "/Windows/ColorDialog/activeComponent", activeRadio );

  //record current scheme
  settings.setValue( "/Windows/ColorDialog/activeScheme", mSchemeComboBox->currentIndex() );

  //record current tab
  settings.setValue( "/Windows/ColorDialog/activeTab", mTabWidget->currentIndex() );

  //record custom colors
  settings.setValue( "/Windows/ColorDialog/customColor1", QVariant( mSwatchButton1->color() ) );
  settings.setValue( "/Windows/ColorDialog/customColor2", QVariant( mSwatchButton2->color() ) );
  settings.setValue( "/Windows/ColorDialog/customColor3", QVariant( mSwatchButton3->color() ) );
  settings.setValue( "/Windows/ColorDialog/customColor4", QVariant( mSwatchButton4->color() ) );
  settings.setValue( "/Windows/ColorDialog/customColor5", QVariant( mSwatchButton5->color() ) );
  settings.setValue( "/Windows/ColorDialog/customColor6", QVariant( mSwatchButton6->color() ) );
  settings.setValue( "/Windows/ColorDialog/customColor7", QVariant( mSwatchButton7->color() ) );
  settings.setValue( "/Windows/ColorDialog/customColor8", QVariant( mSwatchButton8->color() ) );
  settings.setValue( "/Windows/ColorDialog/customColor9", QVariant( mSwatchButton9->color() ) );
  settings.setValue( "/Windows/ColorDialog/customColor10", QVariant( mSwatchButton10->color() ) );
  settings.setValue( "/Windows/ColorDialog/customColor11", QVariant( mSwatchButton11->color() ) );
  settings.setValue( "/Windows/ColorDialog/customColor12", QVariant( mSwatchButton12->color() ) );
  settings.setValue( "/Windows/ColorDialog/customColor13", QVariant( mSwatchButton13->color() ) );
  settings.setValue( "/Windows/ColorDialog/customColor14", QVariant( mSwatchButton14->color() ) );
  settings.setValue( "/Windows/ColorDialog/customColor15", QVariant( mSwatchButton15->color() ) );
  settings.setValue( "/Windows/ColorDialog/customColor16", QVariant( mSwatchButton16->color() ) );

  //sample radius
  settings.setValue( "/Windows/ColorDialog/sampleRadius", mSpinBoxRadius->value() );
}

void QgsCompoundColorWidget::stopPicking( QPoint eventPos, const bool takeSample )
{
  //release mouse and keyboard, and reset cursor
  releaseMouse();
  releaseKeyboard();
  unsetCursor();
  setMouseTracking( false );
  mPickingColor = false;

  if ( !takeSample )
  {
    //not sampling color, nothing more to do
    return;
  }

  //grab snapshot of pixel under mouse cursor
  QColor snappedColor = sampleColor( eventPos );
  mSamplePreview->setColor( snappedColor );
  mColorPreview->setColor( snappedColor, true );
}

void QgsCompoundColorWidget::setColor( const QColor &color )
{
  if ( !color.isValid() )
  {
    return;
  }

  QColor fixedColor = QColor( color );
  if ( !mAllowAlpha )
  {
    //alpha disallowed, so don't permit transparent colors
    fixedColor.setAlpha( 255 );
  }
  QList<QgsColorWidget*> colorWidgets = this->findChildren<QgsColorWidget *>();
  Q_FOREACH ( QgsColorWidget* widget, colorWidgets )
  {
    if ( widget == mSamplePreview )
    {
      continue;
    }
    widget->blockSignals( true );
    widget->setColor( fixedColor );
    widget->blockSignals( false );
  }
  emit currentColorChanged( fixedColor );
}

void QgsCompoundColorWidget::setPreviousColor( const QColor &color )
{
  mOldColorLabel->setVisible( color.isValid() );
  mColorPreview->setColor2( color );
}

void QgsCompoundColorWidget::mousePressEvent( QMouseEvent *e )
{
  if ( mPickingColor )
  {
    //don't show dialog if in color picker mode
    e->accept();
    return;
  }

  QWidget::mousePressEvent( e );
}

QColor QgsCompoundColorWidget::averageColor( const QImage &image ) const
{
  QRgb tmpRgb;
  int colorCount = 0;
  int sumRed = 0;
  int sumBlue = 0;
  int sumGreen = 0;
  //scan through image and sum rgb components
  for ( int heightIndex = 0; heightIndex < image.height(); ++heightIndex )
  {
    QRgb* scanLine = ( QRgb* )image.constScanLine( heightIndex );
    for ( int widthIndex = 0; widthIndex < image.width(); ++widthIndex )
    {
      tmpRgb = scanLine[widthIndex];
      sumRed += qRed( tmpRgb );
      sumBlue += qBlue( tmpRgb );
      sumGreen += qGreen( tmpRgb );
      colorCount++;
    }
  }
  //calculate average components as floats
  double avgRed = ( double )sumRed / ( 255.0 * colorCount );
  double avgGreen = ( double )sumGreen / ( 255.0 * colorCount );
  double avgBlue = ( double )sumBlue / ( 255.0 * colorCount );

  //create a new color representing the average
  return QColor::fromRgbF( avgRed, avgGreen, avgBlue );
}

QColor QgsCompoundColorWidget::sampleColor( QPoint point ) const
{
  int sampleRadius = mSpinBoxRadius->value() - 1;
  QPixmap snappedPixmap = QPixmap::grabWindow( QApplication::desktop()->winId(), point.x() - sampleRadius, point.y() - sampleRadius,
                          1 + sampleRadius * 2, 1 + sampleRadius * 2 );
  QImage snappedImage = snappedPixmap.toImage();
  //scan all pixels and take average color
  return averageColor( snappedImage );
}

void QgsCompoundColorWidget::mouseMoveEvent( QMouseEvent *e )
{
  if ( mPickingColor )
  {
    //currently in color picker mode
    //sample color under cursor update preview widget to give feedback to user
    QColor hoverColor = sampleColor( e->globalPos() );
    mSamplePreview->setColor( hoverColor );

    e->accept();
    return;
  }

  QWidget::mouseMoveEvent( e );
}

void QgsCompoundColorWidget::mouseReleaseEvent( QMouseEvent *e )
{
  if ( mPickingColor )
  {
    //end color picking operation by sampling the color under cursor
    stopPicking( e->globalPos() );
    e->accept();
    return;
  }

  QWidget::mouseReleaseEvent( e );
}

void QgsCompoundColorWidget::keyPressEvent( QKeyEvent *e )
{
  if ( !mPickingColor )
  {
    //if not picking a color, use default tool button behaviour
    QWidget::keyPressEvent( e );
    return;
  }

  //cancel picking, sampling the color if space was pressed
  stopPicking( QCursor::pos(), e->key() == Qt::Key_Space );
}

void QgsCompoundColorWidget::on_mHueRadio_toggled( bool checked )
{
  if ( checked )
  {
    mColorBox->setComponent( QgsColorWidget::Hue );
    mVerticalRamp->setComponent( QgsColorWidget::Hue );
  }
}

void QgsCompoundColorWidget::on_mSaturationRadio_toggled( bool checked )
{
  if ( checked )
  {
    mColorBox->setComponent( QgsColorWidget::Saturation );
    mVerticalRamp->setComponent( QgsColorWidget::Saturation );
  }
}

void QgsCompoundColorWidget::on_mValueRadio_toggled( bool checked )
{
  if ( checked )
  {
    mColorBox->setComponent( QgsColorWidget::Value );
    mVerticalRamp->setComponent( QgsColorWidget::Value );
  }
}

void QgsCompoundColorWidget::on_mRedRadio_toggled( bool checked )
{
  if ( checked )
  {
    mColorBox->setComponent( QgsColorWidget::Red );
    mVerticalRamp->setComponent( QgsColorWidget::Red );
  }
}

void QgsCompoundColorWidget::on_mGreenRadio_toggled( bool checked )
{
  if ( checked )
  {
    mColorBox->setComponent( QgsColorWidget::Green );
    mVerticalRamp->setComponent( QgsColorWidget::Green );
  }
}

void QgsCompoundColorWidget::on_mBlueRadio_toggled( bool checked )
{
  if ( checked )
  {
    mColorBox->setComponent( QgsColorWidget::Blue );
    mVerticalRamp->setComponent( QgsColorWidget::Blue );
  }
}

void QgsCompoundColorWidget::on_mAddColorToSchemeButton_clicked()
{
  mSchemeList->addColor( mColorPreview->color(), QgsSymbolLayerV2Utils::colorToName( mColorPreview->color() ) );
}

void QgsCompoundColorWidget::updateActionsForCurrentScheme()
{
  QgsColorScheme* scheme = mSchemeList->scheme();

  mActionImportColors->setEnabled( scheme->isEditable() );
  mActionPasteColors->setEnabled( scheme->isEditable() );
  mAddColorToSchemeButton->setEnabled( scheme->isEditable() );
  mRemoveColorsFromSchemeButton->setEnabled( scheme->isEditable() );

  QgsUserColorScheme* userScheme = dynamic_cast<QgsUserColorScheme*>( scheme );
  mActionRemovePalette->setEnabled( userScheme ? true : false );
  if ( userScheme )
  {
    mActionShowInButtons->setEnabled( true );
    whileBlocking( mActionShowInButtons )->setChecked( userScheme->flags() & QgsColorScheme::ShowInColorButtonMenu );
  }
  else
  {
    whileBlocking( mActionShowInButtons )->setChecked( false );
    mActionShowInButtons->setEnabled( false );
  }
}
