/***************************************************************************
                           qgscomposerpicture.cpp
                             -------------------
    begin                : September 2005
    copyright            : (C) 2005 by Radim Blazek
    email                : radim.blazek@gmail.com
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include "qgscomposerpicture.h"
#include "qgscomposerutils.h"
#include "qgscomposermap.h"
#include "qgscomposition.h"
#include "qgsatlascomposition.h"
#include "qgsproject.h"
#include "qgsexpression.h"
#include "qgsvectorlayer.h"
#include "qgsmessagelog.h"
#include "qgsdatadefined.h"
#include "qgsnetworkcontentfetcher.h"
#include "qgssymbollayerv2utils.h"
#include "qgssvgcache.h"
#include "qgslogger.h"
#include "qgsbearingutils.h"
#include "qgsmapsettings.h"

#include <QDomDocument>
#include <QDomElement>
#include <QFileInfo>
#include <QImageReader>
#include <QPainter>
#include <QSvgRenderer>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QEventLoop>
#include <QCoreApplication>

QgsComposerPicture::QgsComposerPicture( QgsComposition *composition )
    : QgsComposerItem( composition )
    , mMode( Unknown )
    , mPictureRotation( 0 )
    , mRotationMap( nullptr )
    , mNorthMode( GridNorth )
    , mNorthOffset( 0.0 )
    , mResizeMode( QgsComposerPicture::Zoom )
    , mPictureAnchor( UpperLeft )
    , mSvgFillColor( QColor( 255, 255, 255 ) )
    , mSvgBorderColor( QColor( 0, 0, 0 ) )
    , mSvgBorderWidth( 0.2 )
    , mHasExpressionError( false )
    , mLoadingSvg( false )
{
  mPictureWidth = rect().width();
  init();
}

QgsComposerPicture::QgsComposerPicture()
    : QgsComposerItem( nullptr )
    , mMode( Unknown )
    , mPictureRotation( 0 )
    , mRotationMap( nullptr )
    , mNorthMode( GridNorth )
    , mNorthOffset( 0.0 )
    , mResizeMode( QgsComposerPicture::Zoom )
    , mPictureAnchor( UpperLeft )
    , mSvgFillColor( QColor( 255, 255, 255 ) )
    , mSvgBorderColor( QColor( 0, 0, 0 ) )
    , mSvgBorderWidth( 0.2 )
    , mHasExpressionError( false )
    , mLoadingSvg( false )
{
  mPictureHeight = rect().height();
  init();
}

void QgsComposerPicture::init()
{
  //default to no background
  setBackgroundEnabled( false );

  //data defined strings
  mDataDefinedNames.insert( QgsComposerObject::PictureSource, QString( "dataDefinedSource" ) );

  //insert PictureSource data defined property (only required due to deprecated API elements,
  //remove after 3.0
  setDataDefinedProperty( QgsComposerObject::PictureSource, false, true, QString(), QString() );

  //connect some signals

  //connect to atlas feature changing
  //to update the picture source expression
  connect( &mComposition->atlasComposition(), SIGNAL( featureChanged( QgsFeature* ) ), this, SLOT( refreshPicture() ) );

  //connect to composer print resolution changing
  connect( mComposition, SIGNAL( printResolutionChanged() ), this, SLOT( recalculateSize() ) );
}

QgsComposerPicture::~QgsComposerPicture()
{

}

void QgsComposerPicture::paint( QPainter* painter, const QStyleOptionGraphicsItem* itemStyle, QWidget* pWidget )
{
  Q_UNUSED( itemStyle );
  Q_UNUSED( pWidget );
  if ( !painter )
  {
    return;
  }
  if ( !shouldDrawItem() )
  {
    return;
  }

  drawBackground( painter );

  //int newDpi = ( painter->device()->logicalDpiX() + painter->device()->logicalDpiY() ) / 2;

  //picture resizing
  if ( mMode != Unknown )
  {
    double boundRectWidthMM;
    double boundRectHeightMM;
    QRect imageRect;
    if ( mResizeMode == QgsComposerPicture::Zoom || mResizeMode == QgsComposerPicture::ZoomResizeFrame )
    {
      boundRectWidthMM = mPictureWidth;
      boundRectHeightMM = mPictureHeight;
      imageRect = QRect( 0, 0, mImage.width(), mImage.height() );
    }
    else if ( mResizeMode == QgsComposerPicture::Stretch )
    {
      boundRectWidthMM = rect().width();
      boundRectHeightMM = rect().height();
      imageRect = QRect( 0, 0, mImage.width(), mImage.height() );
    }
    else if ( mResizeMode == QgsComposerPicture::Clip )
    {
      boundRectWidthMM = rect().width();
      boundRectHeightMM = rect().height();
      int imageRectWidthPixels = mImage.width();
      int imageRectHeightPixels = mImage.height();
      imageRect = clippedImageRect( boundRectWidthMM, boundRectHeightMM,
                                    QSize( imageRectWidthPixels, imageRectHeightPixels ) );
    }
    else
    {
      boundRectWidthMM = rect().width();
      boundRectHeightMM = rect().height();
      imageRect = QRect( 0, 0, rect().width() * mComposition->printResolution() / 25.4,
                         rect().height() * mComposition->printResolution() / 25.4 );
    }
    painter->save();
    //antialiasing on
    painter->setRenderHint( QPainter::Antialiasing, true );

    //zoom mode - calculate anchor point and rotation
    if ( mResizeMode == Zoom )
    {
      //TODO - allow placement modes with rotation set. for now, setting a rotation
      //always places picture in center of frame
      if ( !qgsDoubleNear( mPictureRotation, 0.0 ) )
      {
        painter->translate( rect().width() / 2.0, rect().height() / 2.0 );
        painter->rotate( mPictureRotation );
        painter->translate( -boundRectWidthMM / 2.0, -boundRectHeightMM / 2.0 );
      }
      else
      {
        //shift painter to edge/middle of frame depending on placement
        double diffX = rect().width() - boundRectWidthMM;
        double diffY = rect().height() - boundRectHeightMM;

        double dX = 0;
        double dY = 0;
        switch ( mPictureAnchor )
        {
          case UpperLeft:
          case MiddleLeft:
          case LowerLeft:
            //nothing to do
            break;
          case UpperMiddle:
          case Middle:
          case LowerMiddle:
            dX = diffX / 2.0;
            break;
          case UpperRight:
          case MiddleRight:
          case LowerRight:
            dX = diffX;
            break;
        }
        switch ( mPictureAnchor )
        {
          case UpperLeft:
          case UpperMiddle:
          case UpperRight:
            //nothing to do
            break;
          case MiddleLeft:
          case Middle:
          case MiddleRight:
            dY = diffY / 2.0;
            break;
          case LowerLeft:
          case LowerMiddle:
          case LowerRight:
            dY = diffY;
            break;
        }
        painter->translate( dX, dY );
      }
    }
    else if ( mResizeMode == ZoomResizeFrame )
    {
      if ( !qgsDoubleNear( mPictureRotation, 0.0 ) )
      {
        painter->translate( rect().width() / 2.0, rect().height() / 2.0 );
        painter->rotate( mPictureRotation );
        painter->translate( -boundRectWidthMM / 2.0, -boundRectHeightMM / 2.0 );
      }
    }

    if ( mMode == SVG )
    {
      mSVG.render( painter, QRectF( 0, 0, boundRectWidthMM, boundRectHeightMM ) );
    }
    else if ( mMode == RASTER )
    {
      painter->drawImage( QRectF( 0, 0, boundRectWidthMM, boundRectHeightMM ), mImage, imageRect );
    }

    painter->restore();
  }

  //frame and selection boxes
  drawFrame( painter );
  if ( isSelected() )
  {
    drawSelectionBoxes( painter );
  }
}

QRect QgsComposerPicture::clippedImageRect( double &boundRectWidthMM, double &boundRectHeightMM, QSize imageRectPixels )
{
  int boundRectWidthPixels = boundRectWidthMM * mComposition->printResolution() / 25.4;
  int boundRectHeightPixels = boundRectHeightMM * mComposition->printResolution() / 25.4;

  //update boundRectWidth/Height so that they exactly match pixel bounds
  boundRectWidthMM = boundRectWidthPixels * 25.4 / mComposition->printResolution();
  boundRectHeightMM = boundRectHeightPixels * 25.4 / mComposition->printResolution();

  //calculate part of image which fits in bounds
  int leftClip = 0;
  int topClip = 0;

  //calculate left crop
  switch ( mPictureAnchor )
  {
    case UpperLeft:
    case MiddleLeft:
    case LowerLeft:
      leftClip = 0;
      break;
    case UpperMiddle:
    case Middle:
    case LowerMiddle:
      leftClip = ( imageRectPixels.width() - boundRectWidthPixels ) / 2;
      break;
    case UpperRight:
    case MiddleRight:
    case LowerRight:
      leftClip =  imageRectPixels.width() - boundRectWidthPixels;
      break;
  }

  //calculate top crop
  switch ( mPictureAnchor )
  {
    case UpperLeft:
    case UpperMiddle:
    case UpperRight:
      topClip = 0;
      break;
    case MiddleLeft:
    case Middle:
    case MiddleRight:
      topClip = ( imageRectPixels.height() - boundRectHeightPixels ) / 2;
      break;
    case LowerLeft:
    case LowerMiddle:
    case LowerRight:
      topClip = imageRectPixels.height() - boundRectHeightPixels;
      break;
  }

  return QRect( leftClip, topClip, boundRectWidthPixels, boundRectHeightPixels );
}

void QgsComposerPicture::setPictureFile( const QString& path )
{
  setPicturePath( path );
}

void QgsComposerPicture::refreshPicture( const QgsExpressionContext *context )
{
  const QgsExpressionContext* evalContext = context;
  QScopedPointer< QgsExpressionContext > scopedContext;
  if ( !evalContext )
  {
    scopedContext.reset( createExpressionContext() );
    evalContext = scopedContext.data();
  }

  QString source = mSourcePath;

  //data defined source set?
  mHasExpressionError = false;
  QVariant exprVal;
  if ( dataDefinedProperty( QgsComposerObject::PictureSource )->isActive() )
  {
    if ( dataDefinedEvaluate( QgsComposerObject::PictureSource, exprVal, *evalContext ) )
    {
      source = exprVal.toString().trimmed();
      QgsDebugMsg( QString( "exprVal PictureSource:%1" ).arg( source ) );
    }
    else
    {
      mHasExpressionError = true;
      source = QString();
      QgsMessageLog::logMessage( tr( "Picture expression eval error" ) );
    }
  }

  loadPicture( source );
}

void QgsComposerPicture::loadRemotePicture( const QString &url )
{
  //remote location

  QgsNetworkContentFetcher fetcher;
  //pause until HTML fetch
  mLoaded = false;
  fetcher.fetchContent( QUrl( url ) );
  connect( &fetcher, SIGNAL( finished() ), this, SLOT( remotePictureLoaded() ) );

  while ( !mLoaded )
  {
    qApp->processEvents();
  }

  QNetworkReply* reply = fetcher.reply();
  if ( reply )
  {
    QImageReader imageReader( reply );
    mImage = imageReader.read();
    mMode = RASTER;
    reply->deleteLater();
  }
  else
  {
    mMode = Unknown;
  }
}

void QgsComposerPicture::loadLocalPicture( const QString &path )
{
  QFile pic;
  pic.setFileName( path );

  if ( !pic.exists() )
  {
    mMode = Unknown;
  }
  else
  {
    QFileInfo sourceFileInfo( pic );
    QString sourceFileSuffix = sourceFileInfo.suffix();
    if ( sourceFileSuffix.compare( "svg", Qt::CaseInsensitive ) == 0 )
    {
      //try to open svg
      const QByteArray &svgContent = QgsSvgCache::instance()->svgContent( pic.fileName(), rect().width(), mSvgFillColor, mSvgBorderColor, mSvgBorderWidth,
                                     1.0, 1.0 );
      mSVG.load( svgContent );
      if ( mSVG.isValid() )
      {
        mMode = SVG;
        QRect viewBox = mSVG.viewBox(); //take width/height ratio from view box instead of default size
        mDefaultSvgSize.setWidth( viewBox.width() );
        mDefaultSvgSize.setHeight( viewBox.height() );
      }
      else
      {
        mMode = Unknown;
      }
    }
    else
    {
      //try to open raster with QImageReader
      QImageReader imageReader( pic.fileName() );
      if ( imageReader.read( &mImage ) )
      {
        mMode = RASTER;
      }
      else
      {
        mMode = Unknown;
      }
    }
  }

}

void QgsComposerPicture::remotePictureLoaded()
{
  mLoaded = true;
}

void QgsComposerPicture::updateMapRotation()
{
  if ( !mRotationMap )
    return;

  // take map rotation
  double rotation = mRotationMap->mapRotation();

  // handle true north
  switch ( mNorthMode )
  {
    case GridNorth:
      break; // nothing to do

    case TrueNorth:
    {
      QgsPoint center = mRotationMap->currentMapExtent()->center();
      QgsCoordinateReferenceSystem crs = mComposition->mapSettings().destinationCrs();

      try
      {
        double bearing = QgsBearingUtils::bearingTrueNorth( crs, center );
        rotation += bearing;
      }
      catch ( QgsException& e )
      {
        Q_UNUSED( e );
        QgsDebugMsg( QString( "Caught exception %1" ).arg( e.what() ) );
      }
      break;
    }
  }

  rotation += mNorthOffset;
  setPictureRotation( rotation );
}

void QgsComposerPicture::loadPicture( const QString &path )
{
  if ( path.startsWith( "http" ) )
  {
    //remote location
    loadRemotePicture( path );
  }
  else
  {
    //local location
    loadLocalPicture( path );
  }
  if ( mMode != Unknown ) //make sure we start with a new QImage
  {
    recalculateSize();
  }
  else if ( mHasExpressionError || !( path.isEmpty() ) )
  {
    //trying to load an invalid file or bad expression, show cross picture
    mMode = SVG;
    QString badFile( ":/images/composer/missing_image.svg" );
    mSVG.load( badFile );
    if ( mSVG.isValid() )
    {
      mMode = SVG;
      QRect viewBox = mSVG.viewBox(); //take width/height ratio from view box instead of default size
      mDefaultSvgSize.setWidth( viewBox.width() );
      mDefaultSvgSize.setHeight( viewBox.height() );
      recalculateSize();
    }
  }

  emit itemChanged();
}

QRectF QgsComposerPicture::boundedImageRect( double deviceWidth, double deviceHeight )
{
  double imageToDeviceRatio;
  if ( mImage.width() / deviceWidth > mImage.height() / deviceHeight )
  {
    imageToDeviceRatio =  deviceWidth / mImage.width();
    double height = imageToDeviceRatio * mImage.height();
    return QRectF( 0, 0, deviceWidth, height );
  }
  else
  {
    imageToDeviceRatio = deviceHeight / mImage.height();
    double width = imageToDeviceRatio * mImage.width();
    return QRectF( 0, 0, width, deviceHeight );
  }
}

QRectF QgsComposerPicture::boundedSVGRect( double deviceWidth, double deviceHeight )
{
  double imageToSvgRatio;
  if ( deviceWidth / mDefaultSvgSize.width() > deviceHeight / mDefaultSvgSize.height() )
  {
    imageToSvgRatio = deviceHeight / mDefaultSvgSize.height();
    double width = mDefaultSvgSize.width() * imageToSvgRatio;
    return QRectF( 0, 0, width, deviceHeight );
  }
  else
  {
    imageToSvgRatio = deviceWidth / mDefaultSvgSize.width();
    double height = mDefaultSvgSize.height() * imageToSvgRatio;
    return QRectF( 0, 0, deviceWidth, height );
  }
}

QSizeF QgsComposerPicture::pictureSize()
{
  if ( mMode == SVG )
  {
    return mDefaultSvgSize;
  }
  else if ( mMode == RASTER )
  {
    return QSizeF( mImage.width(), mImage.height() );
  }
  else
  {
    return QSizeF( 0, 0 );
  }
}

#if 0
QRectF QgsComposerPicture::boundedSVGRect( double deviceWidth, double deviceHeight )
{
  double imageToSvgRatio;
  if ( deviceWidth / mDefaultSvgSize.width() < deviceHeight / mDefaultSvgSize.height() )
  {
    imageToSvgRatio = deviceWidth / mDefaultSvgSize.width();
    double height = mDefaultSvgSize.height() * imageToSvgRatio;
    return QRectF( 0, 0, deviceWidth, height );
  }
  else
  {
    imageToSvgRatio = deviceHeight / mDefaultSvgSize.height();
    double width = mDefaultSvgSize.width() * imageToSvgRatio;
    return QRectF( 0, 0, width, deviceHeight );
  }
}
#endif //0

void QgsComposerPicture::setSceneRect( const QRectF& rectangle )
{
  QSizeF currentPictureSize = pictureSize();

  if ( mResizeMode == QgsComposerPicture::Clip )
  {
    QgsComposerItem::setSceneRect( rectangle );
    mPictureWidth = rectangle.width();
    mPictureHeight = rectangle.height();
  }
  else
  {
    QRectF newRect = rectangle;

    if ( mResizeMode == ZoomResizeFrame && !rect().isEmpty() && !( currentPictureSize.isEmpty() ) )
    {
      QSizeF targetImageSize;
      if ( qgsDoubleNear( mPictureRotation, 0.0 ) )
      {
        targetImageSize = currentPictureSize;
      }
      else
      {
        //calculate aspect ratio of bounds of rotated image
        QTransform tr;
        tr.rotate( mPictureRotation );
        QRectF rotatedBounds = tr.mapRect( QRectF( 0, 0, currentPictureSize.width(), currentPictureSize.height() ) );
        targetImageSize = QSizeF( rotatedBounds.width(), rotatedBounds.height() );
      }

      //if height has changed more than width, then fix width and set height correspondingly
      //else, do the opposite
      if ( qAbs( rect().width() - rectangle.width() ) <
           qAbs( rect().height() - rectangle.height() ) )
      {
        newRect.setHeight( targetImageSize.height() * newRect.width() / targetImageSize.width() );
      }
      else
      {
        newRect.setWidth( targetImageSize.width() * newRect.height() / targetImageSize.height() );
      }
    }
    else if ( mResizeMode == FrameToImageSize )
    {
      if ( !( currentPictureSize.isEmpty() ) )
      {
        newRect.setWidth( currentPictureSize.width() * 25.4 / mComposition->printResolution() );
        newRect.setHeight( currentPictureSize.height() * 25.4 / mComposition->printResolution() );
      }
    }

    //find largest scaling of picture with this rotation which fits in item
    if ( mResizeMode == Zoom || mResizeMode == ZoomResizeFrame )
    {
      QRectF rotatedImageRect = QgsComposerUtils::largestRotatedRectWithinBounds( QRectF( 0, 0, currentPictureSize.width(), currentPictureSize.height() ), newRect, mPictureRotation );
      mPictureWidth = rotatedImageRect.width();
      mPictureHeight = rotatedImageRect.height();
    }
    else
    {
      mPictureWidth = newRect.width();
      mPictureHeight = newRect.height();
    }

    QgsComposerItem::setSceneRect( newRect );
    emit itemChanged();
  }

  if ( mMode == SVG && !mLoadingSvg )
  {
    mLoadingSvg = true;
    refreshPicture();
    mLoadingSvg = false;
  }
}

void QgsComposerPicture::setRotation( double r )
{
  //kept for compatibility for QGIS2.0 api
  setPictureRotation( r );
}

void QgsComposerPicture::setPictureRotation( double r )
{
  double oldRotation = mPictureRotation;
  mPictureRotation = r;

  if ( mResizeMode == Zoom )
  {
    //find largest scaling of picture with this rotation which fits in item
    QSizeF currentPictureSize = pictureSize();
    QRectF rotatedImageRect = QgsComposerUtils::largestRotatedRectWithinBounds( QRectF( 0, 0, currentPictureSize.width(), currentPictureSize.height() ), rect(), mPictureRotation );
    mPictureWidth = rotatedImageRect.width();
    mPictureHeight = rotatedImageRect.height();
    update();
  }
  else if ( mResizeMode == ZoomResizeFrame )
  {
    QSizeF currentPictureSize = pictureSize();
    QRectF oldRect = QRectF( pos().x(), pos().y(), rect().width(), rect().height() );

    //calculate actual size of image inside frame
    QRectF rotatedImageRect = QgsComposerUtils::largestRotatedRectWithinBounds( QRectF( 0, 0, currentPictureSize.width(), currentPictureSize.height() ), rect(), oldRotation );

    //rotate image rect by new rotation and get bounding box
    QTransform tr;
    tr.rotate( mPictureRotation );
    QRectF newRect = tr.mapRect( QRectF( 0, 0, rotatedImageRect.width(), rotatedImageRect.height() ) );

    //keep the center in the same location
    newRect.moveCenter( oldRect.center() );
    QgsComposerItem::setSceneRect( newRect );
    emit itemChanged();
  }

  emit pictureRotationChanged( mPictureRotation );
}

void QgsComposerPicture::setRotationMap( int composerMapId )
{
  if ( !mComposition )
  {
    return;
  }

  if ( composerMapId == -1 ) //disable rotation from map
  {
    disconnect( mRotationMap, SIGNAL( mapRotationChanged( double ) ), this, SLOT( updateMapRotation() ) );
    disconnect( mRotationMap, SIGNAL( extentChanged() ), this, SLOT( updateMapRotation() ) );
    mRotationMap = nullptr;
  }

  const QgsComposerMap* map = mComposition->getComposerMapById( composerMapId );
  if ( !map )
  {
    return;
  }
  if ( mRotationMap )
  {
    disconnect( mRotationMap, SIGNAL( mapRotationChanged( double ) ), this, SLOT( updateMapRotation() ) );
    disconnect( mRotationMap, SIGNAL( extentChanged() ), this, SLOT( updateMapRotation() ) );
  }
  mPictureRotation = map->mapRotation();
  connect( map, SIGNAL( mapRotationChanged( double ) ), this, SLOT( updateMapRotation() ) );
  connect( map, SIGNAL( extentChanged() ), this, SLOT( updateMapRotation() ) );
  mRotationMap = map;
  updateMapRotation();
  emit pictureRotationChanged( mPictureRotation );
}

void QgsComposerPicture::setResizeMode( QgsComposerPicture::ResizeMode mode )
{
  mResizeMode = mode;
  if ( mode == QgsComposerPicture::ZoomResizeFrame || mode == QgsComposerPicture::FrameToImageSize
       || ( mode == QgsComposerPicture::Zoom && !qgsDoubleNear( mPictureRotation, 0.0 ) ) )
  {
    //call set scene rect to force item to resize to fit picture
    recalculateSize();
  }
  update();
}

void QgsComposerPicture::recalculateSize()
{
  //call set scene rect with current position/size, as this will trigger the
  //picture item to recalculate its frame and image size
  setSceneRect( QRectF( pos().x(), pos().y(), rect().width(), rect().height() ) );
}

void QgsComposerPicture::refreshDataDefinedProperty( const QgsComposerObject::DataDefinedProperty property, const QgsExpressionContext* context )
{
  const QgsExpressionContext* evalContext = context;
  QScopedPointer< QgsExpressionContext > scopedContext;
  if ( !evalContext )
  {
    scopedContext.reset( createExpressionContext() );
    evalContext = scopedContext.data();
  }

  if ( property == QgsComposerObject::PictureSource || property == QgsComposerObject::AllProperties )
  {
    refreshPicture( evalContext );
  }

  QgsComposerItem::refreshDataDefinedProperty( property, evalContext );
}

void QgsComposerPicture::setUsePictureExpression( bool useExpression )
{
  dataDefinedProperty( QgsComposerObject::PictureSource )->setActive( useExpression );
  refreshPicture();
}

void QgsComposerPicture::setPictureExpression( const QString& expression )
{
  dataDefinedProperty( QgsComposerObject::PictureSource )->setExpressionString( expression );
  refreshPicture();
}

QString QgsComposerPicture::pictureFile() const
{
  return picturePath();
}

void QgsComposerPicture::setPicturePath( const QString &path )
{
  mSourcePath = path;
  refreshPicture();
}

QString QgsComposerPicture::picturePath() const
{
  return mSourcePath;
}

bool QgsComposerPicture::writeXML( QDomElement& elem, QDomDocument & doc ) const
{
  if ( elem.isNull() )
  {
    return false;
  }
  QDomElement composerPictureElem = doc.createElement( "ComposerPicture" );
  composerPictureElem.setAttribute( "file", QgsProject::instance()->writePath( mSourcePath ) );
  composerPictureElem.setAttribute( "pictureWidth", QString::number( mPictureWidth ) );
  composerPictureElem.setAttribute( "pictureHeight", QString::number( mPictureHeight ) );
  composerPictureElem.setAttribute( "resizeMode", QString::number( static_cast< int >( mResizeMode ) ) );
  composerPictureElem.setAttribute( "anchorPoint", QString::number( static_cast< int >( mPictureAnchor ) ) );
  composerPictureElem.setAttribute( "svgFillColor", QgsSymbolLayerV2Utils::encodeColor( mSvgFillColor ) );
  composerPictureElem.setAttribute( "svgBorderColor", QgsSymbolLayerV2Utils::encodeColor( mSvgBorderColor ) );
  composerPictureElem.setAttribute( "svgBorderWidth", QString::number( mSvgBorderWidth ) );

  //rotation
  composerPictureElem.setAttribute( "pictureRotation", QString::number( mPictureRotation ) );
  if ( !mRotationMap )
  {
    composerPictureElem.setAttribute( "mapId", -1 );
  }
  else
  {
    composerPictureElem.setAttribute( "mapId", mRotationMap->id() );
  }
  composerPictureElem.setAttribute( "northMode", mNorthMode );
  composerPictureElem.setAttribute( "northOffset", mNorthOffset );

  _writeXML( composerPictureElem, doc );
  elem.appendChild( composerPictureElem );
  return true;
}

bool QgsComposerPicture::readXML( const QDomElement& itemElem, const QDomDocument& doc )
{
  if ( itemElem.isNull() )
  {
    return false;
  }

  mPictureWidth = itemElem.attribute( "pictureWidth", "10" ).toDouble();
  mPictureHeight = itemElem.attribute( "pictureHeight", "10" ).toDouble();
  mResizeMode = QgsComposerPicture::ResizeMode( itemElem.attribute( "resizeMode", "0" ).toInt() );
  //when loading from xml, default to anchor point of middle to match pre 2.4 behaviour
  mPictureAnchor = static_cast< QgsComposerItem::ItemPositionMode >( itemElem.attribute( "anchorPoint", QString::number( QgsComposerItem::Middle ) ).toInt() );

  mSvgFillColor = QgsSymbolLayerV2Utils::decodeColor( itemElem.attribute( "svgFillColor", QgsSymbolLayerV2Utils::encodeColor( QColor( 255, 255, 255 ) ) ) );
  mSvgBorderColor = QgsSymbolLayerV2Utils::decodeColor( itemElem.attribute( "svgBorderColor", QgsSymbolLayerV2Utils::encodeColor( QColor( 0, 0, 0 ) ) ) );
  mSvgBorderWidth = itemElem.attribute( "svgBorderWidth", "0.2" ).toDouble();

  QDomNodeList composerItemList = itemElem.elementsByTagName( "ComposerItem" );
  if ( !composerItemList.isEmpty() )
  {
    QDomElement composerItemElem = composerItemList.at( 0 ).toElement();

    if ( !qgsDoubleNear( composerItemElem.attribute( "rotation", "0" ).toDouble(), 0.0 ) )
    {
      //in versions prior to 2.1 picture rotation was stored in the rotation attribute
      mPictureRotation = composerItemElem.attribute( "rotation", "0" ).toDouble();
    }

    _readXML( composerItemElem, doc );
  }

  mDefaultSvgSize = QSize( 0, 0 );

  if ( itemElem.hasAttribute( "sourceExpression" ) )
  {
    //update pre 2.5 picture expression to use data defined expression
    QString sourceExpression = itemElem.attribute( "sourceExpression", "" );
    QString useExpression = itemElem.attribute( "useExpression" );
    bool expressionActive;
    if ( useExpression.compare( "true", Qt::CaseInsensitive ) == 0 )
    {
      expressionActive = true;
    }
    else
    {
      expressionActive = false;
    }

    setDataDefinedProperty( QgsComposerObject::PictureSource, expressionActive, true, sourceExpression, QString() );
  }

  mSourcePath = QgsProject::instance()->readPath( itemElem.attribute( "file" ) );

  //picture rotation
  if ( !qgsDoubleNear( itemElem.attribute( "pictureRotation", "0" ).toDouble(), 0.0 ) )
  {
    mPictureRotation = itemElem.attribute( "pictureRotation", "0" ).toDouble();
  }

  //rotation map
  mNorthMode = static_cast< NorthMode >( itemElem.attribute( "northMode", "0" ).toInt() );
  mNorthOffset = itemElem.attribute( "northOffset", "0" ).toDouble();

  int rotationMapId = itemElem.attribute( "mapId", "-1" ).toInt();
  if ( rotationMapId == -1 )
  {
    mRotationMap = nullptr;
  }
  else if ( mComposition )
  {

    if ( mRotationMap )
    {
      disconnect( mRotationMap, SIGNAL( mapRotationChanged( double ) ), this, SLOT( updateMapRotation() ) );
      disconnect( mRotationMap, SIGNAL( extentChanged() ), this, SLOT( updateMapRotation() ) );
    }
    mRotationMap = mComposition->getComposerMapById( rotationMapId );
    connect( mRotationMap, SIGNAL( mapRotationChanged( double ) ), this, SLOT( updateMapRotation() ) );
    connect( mRotationMap, SIGNAL( extentChanged() ), this, SLOT( updateMapRotation() ) );
  }

  refreshPicture();

  emit itemChanged();
  return true;
}

int QgsComposerPicture::rotationMap() const
{
  if ( !mRotationMap )
  {
    return -1;
  }
  else
  {
    return mRotationMap->id();
  }
}

void QgsComposerPicture::setNorthMode( QgsComposerPicture::NorthMode mode )
{
  mNorthMode = mode;
  updateMapRotation();
}

void QgsComposerPicture::setNorthOffset( double offset )
{
  mNorthOffset = offset;
  updateMapRotation();
}

void QgsComposerPicture::setPictureAnchor( QgsComposerItem::ItemPositionMode anchor )
{
  mPictureAnchor = anchor;
  update();
}

void QgsComposerPicture::setSvgFillColor( const QColor& color )
{
  mSvgFillColor = color;
  refreshPicture();
}

void QgsComposerPicture::setSvgBorderColor( const QColor& color )
{
  mSvgBorderColor = color;
  refreshPicture();
}

void QgsComposerPicture::setSvgBorderWidth( double width )
{
  mSvgBorderWidth = width;
  refreshPicture();
}

bool QgsComposerPicture::usePictureExpression() const
{
  return dataDefinedProperty( QgsComposerObject::PictureSource )->isActive();
}

QString QgsComposerPicture::pictureExpression() const
{
  return dataDefinedProperty( QgsComposerObject::PictureSource )->expressionString();
}

bool QgsComposerPicture::imageSizeConsideringRotation( double& width, double& height ) const
{
  //kept for api compatibility with QGIS 2.0 - use mPictureRotation
  Q_NOWARN_DEPRECATED_PUSH
  return QgsComposerItem::imageSizeConsideringRotation( width, height, mPictureRotation );
  Q_NOWARN_DEPRECATED_POP
}

bool QgsComposerPicture::cornerPointOnRotatedAndScaledRect( double& x, double& y, double width, double height ) const
{
  //kept for api compatibility with QGIS 2.0 - use mPictureRotation
  Q_NOWARN_DEPRECATED_PUSH
  return QgsComposerItem::cornerPointOnRotatedAndScaledRect( x, y, width, height, mPictureRotation );
  Q_NOWARN_DEPRECATED_POP
}

void QgsComposerPicture::sizeChangedByRotation( double& width, double& height )
{
  //kept for api compatibility with QGIS 2.0 - use mPictureRotation
  Q_NOWARN_DEPRECATED_PUSH
  return QgsComposerItem::sizeChangedByRotation( width, height, mPictureRotation );
  Q_NOWARN_DEPRECATED_POP
}
