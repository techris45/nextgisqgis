/***************************************************************************
    qgsvectordataprovider.h - DataProvider Interface for vector layers
     --------------------------------------
    Date                 : 23-Sep-2004
    Copyright            : (C) 2004 by Marco Hugentobler
    email                : marco.hugentobler@autoform.ch
 ***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/
#ifndef QGSVECTORDATAPROVIDER_H
#define QGSVECTORDATAPROVIDER_H

class QTextCodec;

#include <QList>
#include <QSet>
#include <QMap>
#include <QHash>

//QGIS Includes
#include "qgis.h"
#include "qgsdataprovider.h"
#include "qgsfeature.h"
#include "qgsfield.h"
#include "qgsrectangle.h"
#include "qgsaggregatecalculator.h"

typedef QList<int> QgsAttributeList;
typedef QSet<int> QgsAttributeIds;
typedef QHash<int, QString> QgsAttrPalIndexNameHash;

class QgsFeatureIterator;
class QgsTransaction;

#include "qgsfeaturerequest.h"

/** \ingroup core
 * This is the base class for vector data providers.
 *
 * Data providers abstract the retrieval and writing (where supported)
 * of feature and attribute information from a spatial datasource.
 *
 *
 */
class CORE_EXPORT QgsVectorDataProvider : public QgsDataProvider
{
    Q_OBJECT

    friend class QgsTransaction;
    friend class QgsVectorLayerEditBuffer;

  public:

    // If you add to this, please also add to capabilitiesString()
    /**
     * enumeration with capabilities that providers might implement
     */
    enum Capability
    {
      /** Provider has no capabilities */
      NoCapabilities =                              0,
      /** Allows adding features */
      AddFeatures =                                 1,
      /** Allows deletion of features */
      DeleteFeatures =                              1 <<  1,
      /** Allows modification of attribute values */
      ChangeAttributeValues =                       1 <<  2,
      /** Allows addition of new attributes (fields) */
      AddAttributes =                               1 <<  3,
      /** Allows deletion of attributes (fields) */
      DeleteAttributes =                            1 <<  4,
      /** DEPRECATED - do not use */
      SaveAsShapefile =                             1 <<  5,
      /** Allows creation of spatial index */
      CreateSpatialIndex =                          1 <<  6,
      /** Fast access to features using their ID */
      SelectAtId =                                  1 <<  7,
      /** Allows modifications of geometries */
      ChangeGeometries =                            1 <<  8,
      /** DEPRECATED - do not use */
      SelectGeometryAtId =                          1 <<  9,
      /** DEPRECATED - do not use */
      RandomSelectGeometryAtId =                    1 << 10,
      /** DEPRECATED - do not use */
      SequentialSelectGeometryAtId =                1 << 11,
      /** DEPRECATED - do not use */
      CreateAttributeIndex =                        1 << 12,
      /** Allows user to select encoding */
      SelectEncoding =                              1 << 13,
      /** Supports simplification of geometries on provider side according to a distance tolerance */
      SimplifyGeometries =                          1 << 14,
      /** Supports topological simplification of geometries on provider side according to a distance tolerance */
      SimplifyGeometriesWithTopologicalValidation = 1 << 15,
      /** Supports transactions*/
      TransactionSupport =                          1 << 16,
      /** Supports circular geometry types (circularstring, compoundcurve, curvepolygon)*/
      CircularGeometries =                          1 << 17,
      /** Supports joint updates for attributes and geometry
       * Providers supporting this should still define ChangeGeometries | ChangeAttributeValues
       */
      ChangeFeatures =                              1 << 18,
      /** Supports renaming attributes (fields). Added in QGIS 2.16 */
      RenameAttributes =                            1 << 19,
    };

    /** Bitmask of all provider's editing capabilities */
    const static int EditingCapabilities = AddFeatures | DeleteFeatures |
                                           ChangeAttributeValues | ChangeGeometries | AddAttributes | DeleteAttributes |
                                           RenameAttributes;

    /**
     * Constructor of the vector provider
     * @param uri  uniform resource locator (URI) for a dataset
     */
    QgsVectorDataProvider( const QString& uri = QString() );

    /**
     * Destructor
     */
    virtual ~QgsVectorDataProvider();

    /**
     * Return feature source object that can be used for querying provider's data. The returned feature source
     * is independent from provider - any changes to provider's state (e.g. change of subset string) will not be
     * reflected in the feature source, therefore it can be safely used for processing in background without
     * having to care about possible changes within provider that may happen concurrently. Also, even in the case
     * of provider being deleted, any feature source obtained from the provider will be kept alive and working
     * (they are independent and owned by the caller).
     *
     * Sometimes there are cases when some data needs to be shared between vector data provider and its feature source.
     * In such cases, the implementation must ensure that the data is not susceptible to run condition. For example,
     * if it is possible that both feature source and provider may need reading/writing to some shared data at the
     * same time, some synchronization mechanisms must be used (e.g. mutexes) to prevent data corruption.
     *
     * @note added in 2.4
     * @return new instance of QgsAbstractFeatureSource (caller is responsible for deleting it)
     */
    virtual QgsAbstractFeatureSource *featureSource() const = 0;

    /**
     * Returns the permanent storage type for this layer as a friendly name.
     */
    virtual QString storageType() const;

    /**
     * Query the provider for features specified in request.
     */
    virtual QgsFeatureIterator getFeatures( const QgsFeatureRequest& request = QgsFeatureRequest() ) = 0;

    /**
     * Get feature type.
     * @return int representing the feature type
     */
    virtual QGis::WkbType geometryType() const = 0;

    /**
     * Number of features in the layer
     * @return long containing number of features
     */
    virtual long featureCount() const = 0;

    /**
     * Return a map of indexes with field names for this layer
     * @return map of fields
     * @see QgsFields
     */
    // TODO QGIS 3: return by value
    virtual const QgsFields &fields() const = 0;

    /**
     * Return a short comment for the data that this provider is
     * providing access to (e.g. the comment for postgres table).
     */
    virtual QString dataComment() const;

    /**
     * Returns the minimum value of an attribute
     * @param index the index of the attribute
     *
     * Default implementation walks all numeric attributes and caches minimal
     * and maximal values. If provider has facilities to retrieve minimal
     * value directly, override this function.
     */
    virtual QVariant minimumValue( int index );

    /**
     * Returns the maximum value of an attribute
     * @param index the index of the attribute
     *
     * Default implementation walks all numeric attributes and caches minimal
     * and maximal values. If provider has facilities to retrieve maximal
     * value directly, override this function.
     */
    virtual QVariant maximumValue( int index );

    /**
     * Return unique values of an attribute
     * @param index the index of the attribute
     * @param uniqueValues values reference to the list to fill
     * @param limit maxmum number of the values to return
     *
     * Default implementation simply iterates the features
     */
    virtual void uniqueValues( int index, QList<QVariant> &uniqueValues, int limit = -1 );

    /** Calculates an aggregated value from the layer's features. The base implementation does nothing,
     * but subclasses can override this method to handoff calculation of aggregates to the provider.
     * @param aggregate aggregate to calculate
     * @param index the index of the attribute to calculate aggregate over
     * @param parameters parameters controlling aggregate calculation
     * @param context expression context for filter
     * @param ok will be set to true if calculation was successfully performed by the data provider
     * @return calculated aggregate value
     * @note added in QGIS 2.16
     */
    virtual QVariant aggregate( QgsAggregateCalculator::Aggregate aggregate,
                                int index,
                                const QgsAggregateCalculator::AggregateParameters& parameters,
                                QgsExpressionContext* context,
                                bool& ok );

    /**
     * Returns the possible enum values of an attribute. Returns an empty stringlist if a provider does not support enum types
     * or if the given attribute is not an enum type.
     * @param index the index of the attribute
     * @param enumList reference to the list to fill
     */
    virtual void enumValues( int index, QStringList& enumList ) { Q_UNUSED( index ); enumList.clear(); }

    /**
     * Adds a list of features
     * @return true in case of success and false in case of failure
     */
    virtual bool addFeatures( QgsFeatureList &flist );

    /**
     * Deletes one or more features
     * @param id list containing feature ids to delete
     * @return true in case of success and false in case of failure
     */
    virtual bool deleteFeatures( const QgsFeatureIds &id );

    /**
     * Adds new attributes
     * @param attributes list of new attributes
     * @return true in case of success and false in case of failure
     */
    virtual bool addAttributes( const QList<QgsField> &attributes );

    /**
     * Deletes existing attributes
     * @param attributes a set containing indices of attributes
     * @return true in case of success and false in case of failure
     */
    virtual bool deleteAttributes( const QgsAttributeIds &attributes );

    /**
     * Renames existing attributes.
     * @param renamedAttributes map of attribute index to new attribute name
     * @return true in case of success and false in case of failure
     * @note added in QGIS 2.16
     */
    virtual bool renameAttributes( const QgsFieldNameMap& renamedAttributes );

    /**
     * Changes attribute values of existing features.
     * @param attr_map a map containing changed attributes
     * @return true in case of success and false in case of failure
     */
    virtual bool changeAttributeValues( const QgsChangedAttributesMap &attr_map );

    /**
     * Changes attribute values and geometries of existing features.
     * @param attr_map a map containing changed attributes
     * @param geometry_map   A QgsGeometryMap whose index contains the feature IDs
     *                       that will have their geometries changed.
     *                       The second map parameter being the new geometries themselves
     * @return true in case of success and false in case of failure
     */
    virtual bool changeFeatures( const QgsChangedAttributesMap &attr_map,
                                 const QgsGeometryMap &geometry_map );

    /**
     * Returns the default value for field specified by \a fieldId.
     * If \a forceLazyEval is set to true, the provider the default value
     * will not be evaluated on server side even if specified in the project properties.
     */
    virtual QVariant defaultValue( int fieldId, bool forceLazyEval = false );

    /**
     * Changes geometries of existing features
     * @param geometry_map   A QgsGeometryMap whose index contains the feature IDs
     *                       that will have their geometries changed.
     *                       The second map parameter being the new geometries themselves
     * @return               True in case of success and false in case of failure
     */
    virtual bool changeGeometryValues( const QgsGeometryMap &geometry_map );

    /**
     * Creates a spatial index on the datasource (if supported by the provider type).
     * @return true in case of success
     */
    virtual bool createSpatialIndex();

    /** Create an attribute index on the datasource*/
    virtual bool createAttributeIndex( int field );

    /** Returns a bitmask containing the supported capabilities
        Note, some capabilities may change depending on whether
        a spatial filter is active on this provider, so it may
        be prudent to check this value per intended operation.
     */
    virtual int capabilities() const;

    /**
     *  Returns the above in friendly format.
     */
    QString capabilitiesString() const;

    /**
     * Set encoding used for accessing data from layer
     */
    virtual void setEncoding( const QString& e );

    /**
     * Get encoding which is used for accessing data
     */
    QString encoding() const;

    /**
     * Returns the index of a field name or -1 if the field does not exist
     */
    int fieldNameIndex( const QString& fieldName ) const;

    /**
     * Return a map where the key is the name of the field and the value is its index
     */
    QMap<QString, int> fieldNameMap() const;

    /**
     * Return list of indexes to fetch all attributes in nextFeature()
     */
    virtual QgsAttributeList attributeIndexes();

    /**
     * Return list of indexes of fields that make up the primary key
     */
    virtual QgsAttributeList pkAttributeIndexes() { return QgsAttributeList(); }

    /**
     * Return list of indexes to names for QgsPalLabeling fix
     */
    virtual QgsAttrPalIndexNameHash palAttributeIndexNames() const { return mAttrPalIndexName; }

    /**
     * check if provider supports type of field
     */
    bool supportedType( const QgsField &field ) const;

    struct NativeType
    {
      NativeType( const QString& typeDesc, const QString& typeName, QVariant::Type type, int minLen = 0, int maxLen = 0, int minPrec = 0, int maxPrec = 0 )
          : mTypeDesc( typeDesc )
          , mTypeName( typeName )
          , mType( type )
          , mMinLen( minLen )
          , mMaxLen( maxLen )
          , mMinPrec( minPrec )
          , mMaxPrec( maxPrec )
      {}

      QString mTypeDesc;
      QString mTypeName;
      QVariant::Type mType;
      int mMinLen;
      int mMaxLen;
      int mMinPrec;
      int mMaxPrec;
    };

    /**
     * Returns the names of the supported types
     */
    const QList< NativeType > &nativeTypes() const;

    /**
     * Returns true if the provider is strict about the type of inserted features
     * (e.g. no multipolygon in a polygon layer)
     */
    virtual bool doesStrictFeatureTypeCheck() const { return true;}

    /** Returns a list of available encodings */
    static const QStringList &availableEncodings();

    /**
     * Provider has errors to report
     */
    bool hasErrors();

    /**
     * Clear recorded errors
     */
    void clearErrors();

    /**
     * Get recorded errors
     */
    QStringList errors();


    /**
     * It returns false by default.
     * Must be implemented by providers that support saving and loading styles to db returning true
     */
    virtual bool isSaveAndLoadStyleToDBSupported() { return false; }

    static QVariant convertValue( QVariant::Type type, const QString& value );

    /**
     * Returns the transaction this data provider is included in, if any.
     */
    virtual QgsTransaction* transaction() const { return nullptr; }

    /**
     * Forces a reload of the underlying datasource if the provider implements this
     * method.
     * In particular on the OGR provider, a pooled connection will be invalidated.
     * This forces QGIS to reopen a file or connection.
     * This can be required if the underlying file is replaced.
     */
    virtual void forceReload()
    {
      emit dataChanged();
    }

    /**
     * Get the list of layer ids on which this layer depends. This in particular determines the order of layer loading.
     */
    virtual QSet<QString> layerDependencies() const;

  signals:
    /** Signals an error in this provider */
    void raiseError( const QString& msg );

  protected:
    void clearMinMaxCache();
    void fillMinMaxCache();

    bool mCacheMinMaxDirty;
    QMap<int, QVariant> mCacheMinValues, mCacheMaxValues;

    /** Encoding */
    QTextCodec* mEncoding;

    /** List of attribute indices to fetch with nextFeature calls*/
    QgsAttributeList mAttributesToFetch;

    /** The names of the providers native types*/
    QList< NativeType > mNativeTypes;

    void pushError( const QString& msg );

    /** Old-style mapping of index to name for QgsPalLabeling fix */
    QgsAttrPalIndexNameHash mAttrPalIndexName;

    /** \brief Converts the geometry to the provider type if possible / necessary
     * this is the list of possible modifications:
     * - convert compoundcurve to circularstring
     *   (possible if compoundcurve consists of one circular string)
     * - convert to multitype if necessary
     * - convert to curved type if necessary
     * - convert to linear type from curved type
     * - Add z/m 0 default values
     * - Remove z/m
     * \ref QgsVectorLayerEditBuffer::adaptGeometry()
     * \param geom Geometry to convert
     * \returns the converted geometry or nullptr if no conversion was necessary or possible*/
    QgsGeometry* convertToProviderType( const QgsGeometry* geom ) const;

  private:
    /** Old notation **/
    QMap<QString, QVariant::Type> mOldTypeList;

    /** List of errors */
    QStringList mErrors;

    static QStringList smEncodings;

    /**
     * Includes this data provider in the specified transaction. Ownership of transaction is not transferred.
     */
    virtual void setTransaction( QgsTransaction* /*transaction*/ ) {}

};


#endif
