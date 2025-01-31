/***************************************************************************
    qgsvectorlayereditbuffer.h
    ---------------------
    begin                : Dezember 2012
    copyright            : (C) 2012 by Martin Dobias
    email                : wonder dot sk at gmail dot com
 ***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/
#ifndef QGSVECTORLAYEREDITBUFFER_H
#define QGSVECTORLAYEREDITBUFFER_H

#include <QList>
#include <QSet>

#include "qgsgeometry.h"
#include "qgsfeature.h"
#include "qgsfield.h"
#include "qgsrectangle.h"
#include "qgssnapper.h"

class QgsVectorLayer;

typedef QList<int> QgsAttributeList;
typedef QSet<int> QgsAttributeIds;
typedef QMap<QgsFeatureId, QgsFeature> QgsFeatureMap;

/** \ingroup core
 * \class QgsVectorLayerEditBuffer
 */
class CORE_EXPORT QgsVectorLayerEditBuffer : public QObject
{
    Q_OBJECT
  public:
    QgsVectorLayerEditBuffer( QgsVectorLayer* layer );
    ~QgsVectorLayerEditBuffer();

    /** Returns true if the provider has been modified since the last commit */
    virtual bool isModified() const;


    /** Adds a feature
        @param f feature to add
        @return True in case of success and False in case of error
     */
    virtual bool addFeature( QgsFeature& f );

    /** Insert a copy of the given features into the layer  (but does not commit it) */
    virtual bool addFeatures( QgsFeatureList& features );

    /** Delete a feature from the layer (but does not commit it) */
    virtual bool deleteFeature( QgsFeatureId fid );

    /** Deletes a set of features from the layer (but does not commit it) */
    virtual bool deleteFeatures( const QgsFeatureIds& fid );

    /** Change feature's geometry */
    virtual bool changeGeometry( QgsFeatureId fid, QgsGeometry* geom );

    /** Changed an attribute value (but does not commit it) */
    virtual bool changeAttributeValue( QgsFeatureId fid, int field, const QVariant &newValue, const QVariant &oldValue = QVariant() );

    /** Add an attribute field (but does not commit it)
        returns true if the field was added */
    virtual bool addAttribute( const QgsField &field );

    /** Delete an attribute field (but does not commit it) */
    virtual bool deleteAttribute( int attr );

    /** Renames an attribute field (but does not commit it)
     * @param attr attribute index
     * @param newName new name of field
     * @note added in QGIS 2.16
    */
    virtual bool renameAttribute( int attr, const QString& newName );

    /**
      Attempts to commit any changes to disk.  Returns the result of the attempt.
      If a commit fails, the in-memory changes are left alone.

      This allows editing to continue if the commit failed on e.g. a
      disallowed value in a Postgres database - the user can re-edit and try
      again.

      The commits occur in distinct stages,
      (add attributes, add features, change attribute values, change
      geometries, delete features, delete attributes)
      so if a stage fails, it's difficult to roll back cleanly.
      Therefore any error message also includes which stage failed so
      that the user has some chance of repairing the damage cleanly.
     */
    virtual bool commitChanges( QStringList& commitErrors );

    /** Stop editing and discard the edits */
    virtual void rollBack();

    /**
     * Changes values of attributes (but does not commit it).
     * @return true if attributes are well updated, false otherwise
     * @note added in QGIS 2.18
     */
    virtual bool changeAttributeValues( QgsFeatureId fid, const QgsAttributeMap &newValues, const QgsAttributeMap &oldValues );

    /** New features which are not commited. */
    inline const QgsFeatureMap& addedFeatures() { return mAddedFeatures; }

    /** Changed attributes values which are not commited */
    inline const QgsChangedAttributesMap& changedAttributeValues() { return mChangedAttributeValues; }

    /** Deleted attributes fields which are not commited. The list is kept sorted. */
    inline const QgsAttributeList& deletedAttributeIds() { return mDeletedAttributeIds; }

    /** Added attributes fields which are not commited */
    inline const QList<QgsField>& addedAttributes() { return mAddedAttributes; }

    /** Changed geometries which are not commited. */
    inline const QgsGeometryMap& changedGeometries() { return mChangedGeometries; }

    inline const QgsFeatureIds deletedFeatureIds() { return mDeletedFeatureIds; }
    //QString dumpEditBuffer();

  protected slots:
    void undoIndexChanged( int index );

  signals:
    /** This signal is emitted when modifications has been done on layer */
    void layerModified();

    void featureAdded( QgsFeatureId fid );
    void featureDeleted( QgsFeatureId fid );
    void geometryChanged( QgsFeatureId fid, QgsGeometry &geom );
    void attributeValueChanged( QgsFeatureId fid, int idx, const QVariant & );
    void attributeAdded( int idx );
    void attributeDeleted( int idx );

    /** Emitted when an attribute has been renamed
     * @param idx attribute index
     * @param newName new attribute name
     * @note added in QGSI 2.16
     */
    void attributeRenamed( int idx, const QString& newName );

    /** Signals emitted after committing changes */
    void committedAttributesDeleted( const QString& layerId, const QgsAttributeList& deletedAttributes );
    void committedAttributesAdded( const QString& layerId, const QList<QgsField>& addedAttributes );

    /** Emitted after committing an attribute rename
     * @param layerId ID of layer
     * @param renamedAttributes map of field index to new name
     * @note added in QGIS 2.16
     */
    void committedAttributesRenamed( const QString& layerId, const QgsFieldNameMap& renamedAttributes );
    void committedFeaturesAdded( const QString& layerId, const QgsFeatureList& addedFeatures );
    void committedFeaturesRemoved( const QString& layerId, const QgsFeatureIds& deletedFeatureIds );
    void committedAttributeValuesChanges( const QString& layerId, const QgsChangedAttributesMap& changedAttributesValues );
    void committedGeometriesChanges( const QString& layerId, const QgsGeometryMap& changedGeometries );

  protected:

    QgsVectorLayerEditBuffer() : L( nullptr ) {}

    void updateFields( QgsFields& fields );

    /** Update feature with uncommited geometry updates */
    void updateFeatureGeometry( QgsFeature &f );

    /** Update feature with uncommited attribute updates */
    void updateChangedAttributes( QgsFeature &f );

    /** Update added and changed features after addition of an attribute */
    void handleAttributeAdded( int index );

    /** Update added and changed features after removal of an attribute */
    void handleAttributeDeleted( int index );

    /** Updates an index in an attribute map to a new value (for updates of changed attributes) */
    void updateAttributeMapIndex( QgsAttributeMap& attrs, int index, int offset ) const;

    void updateLayerFields();

    /** \brief Apply geometry modification basing on provider geometry type.
     * Geometry is modified only if successful conversion is possible.
     * adaptGeometry calls \ref QgsVectorDataProvider::convertToProviderType()
     * if necessary and that apply the modifications.
     * \param geometry pointer to the geometry that should be adapted to provider
     * \returns bool true if success.
     *  True: Input geometry is changed because conversion is applied or
     *  geometry is untouched if geometry conversion is not necessary.
     *  False: Conversion is not possible and geometry is untouched.
     * \note added in QGIS 2.18.14
     */
    bool adaptGeometry( QgsGeometry* geometry );

  protected:
    QgsVectorLayer* L;
    friend class QgsVectorLayer;

    friend class QgsVectorLayerUndoCommand;
    friend class QgsVectorLayerUndoCommandAddFeature;
    friend class QgsVectorLayerUndoCommandDeleteFeature;
    friend class QgsVectorLayerUndoCommandChangeGeometry;
    friend class QgsVectorLayerUndoCommandChangeAttribute;
    friend class QgsVectorLayerUndoCommandAddAttribute;
    friend class QgsVectorLayerUndoCommandDeleteAttribute;
    friend class QgsVectorLayerUndoCommandRenameAttribute;

    /** Deleted feature IDs which are not commited.  Note a feature can be added and then deleted
        again before the change is committed - in that case the added feature would be removed
        from mAddedFeatures only and *not* entered here.
     */
    QgsFeatureIds mDeletedFeatureIds;

    /** New features which are not commited. */
    QgsFeatureMap mAddedFeatures;

    /** Changed attributes values which are not commited */
    QgsChangedAttributesMap mChangedAttributeValues;

    /** Deleted attributes fields which are not commited. The list is kept sorted. */
    QgsAttributeList mDeletedAttributeIds;

    /** Added attributes fields which are not commited */
    QList<QgsField> mAddedAttributes;

    /** Renamed attributes which are not commited. */
    QgsFieldNameMap mRenamedAttributes;

    /** Changed geometries which are not commited. */
    QgsGeometryMap mChangedGeometries;
};

#endif // QGSVECTORLAYEREDITBUFFER_H
