/***************************************************************************
    qgsgeometryanglecheck.cpp
    ---------------------
    begin                : September 2015
    copyright            : (C) 2014 by Sandro Mani / Sourcepole AG
    email                : smani at sourcepole dot ch
 ***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include "qgsgeometryanglecheck.h"
#include "qgsgeometryutils.h"
#include "qgsfeaturepool.h"

void QgsGeometryAngleCheck::collectErrors( QList<QgsGeometryCheckError *> &errors, QStringList &/*messages*/, QAtomicInt *progressCounter, const QMap<QString, QgsFeatureIds> &ids ) const
{
  QMap<QString, QgsFeatureIds> featureIds = ids.isEmpty() ? allLayerFeatureIds() : ids;
  QgsGeometryCheckerUtils::LayerFeatures layerFeatures( mContext->featurePools, featureIds, mCompatibleGeometryTypes, progressCounter );
  for ( const QgsGeometryCheckerUtils::LayerFeature &layerFeature : layerFeatures )
  {
    const QgsAbstractGeometry *geom = layerFeature.geometry();
    for ( int iPart = 0, nParts = geom->partCount(); iPart < nParts; ++iPart )
    {
      for ( int iRing = 0, nRings = geom->ringCount( iPart ); iRing < nRings; ++iRing )
      {
        bool closed = false;
        int nVerts = QgsGeometryCheckerUtils::polyLineSize( geom, iPart, iRing, &closed );
        // Less than three points, no angles to check
        if ( nVerts < 3 )
        {
          continue;
        }
        for ( int iVert = !closed; iVert < nVerts - !closed; ++iVert )
        {
          const QgsPoint &p1 = geom->vertexAt( QgsVertexId( iPart, iRing, ( iVert - 1 + nVerts ) % nVerts ) );
          const QgsPoint &p2 = geom->vertexAt( QgsVertexId( iPart, iRing, iVert ) );
          const QgsPoint &p3 = geom->vertexAt( QgsVertexId( iPart, iRing, ( iVert + 1 ) % nVerts ) );
          QgsVector v21, v23;
          try
          {
            v21 = QgsVector( p1.x() - p2.x(), p1.y() - p2.y() ).normalized();
            v23 = QgsVector( p3.x() - p2.x(), p3.y() - p2.y() ).normalized();
          }
          catch ( const QgsException & )
          {
            // Zero length vectors
            continue;
          }

          double angle = qAcos( v21 * v23 ) / M_PI * 180.0;
          if ( angle < mMinAngle )
          {
            errors.append( new QgsGeometryCheckError( this, layerFeature, p2, QgsVertexId( iPart, iRing, iVert ), angle ) );
          }
        }
      }
    }
  }
}

void QgsGeometryAngleCheck::fixError( QgsGeometryCheckError *error, int method, const QMap<QString, int> & /*mergeAttributeIndices*/, Changes &changes ) const
{
  QgsFeaturePool *featurePool = mContext->featurePools[ error->layerId() ];
  QgsFeature feature;
  if ( !featurePool->get( error->featureId(), feature ) )
  {
    error->setObsolete();
    return;
  }
  QgsGeometry featureGeometry = feature.geometry();
  QgsAbstractGeometry *geometry = featureGeometry.get();
  QgsVertexId vidx = error->vidx();

  // Check if point still exists
  if ( !vidx.isValid( geometry ) )
  {
    error->setObsolete();
    return;
  }

  // Check if error still applies
  int n = QgsGeometryCheckerUtils::polyLineSize( geometry, vidx.part, vidx.ring );
  if ( n == 0 )
  {
    error->setObsolete();
    return;
  }
  const QgsPoint &p1 = geometry->vertexAt( QgsVertexId( vidx.part, vidx.ring, ( vidx.vertex - 1 + n ) % n ) );
  const QgsPoint &p2 = geometry->vertexAt( vidx );
  const QgsPoint &p3 = geometry->vertexAt( QgsVertexId( vidx.part, vidx.ring, ( vidx.vertex + 1 ) % n ) );
  QgsVector v21, v23;
  try
  {
    v21 = QgsVector( p1.x() - p2.x(), p1.y() - p2.y() ).normalized();
    v23 = QgsVector( p3.x() - p2.x(), p3.y() - p2.y() ).normalized();
  }
  catch ( const QgsException & )
  {
    error->setObsolete();
    return;
  }
  double angle = std::acos( v21 * v23 ) / M_PI * 180.0;
  if ( angle >= mMinAngle )
  {
    error->setObsolete();
    return;
  }

  // Fix error
  if ( method == NoChange )
  {
    error->setFixed( method );
  }
  else if ( method == DeleteNode )
  {
    if ( !QgsGeometryCheckerUtils::canDeleteVertex( geometry, vidx.part, vidx.ring ) )
    {
      error->setFixFailed( tr( "Resulting geometry is degenerate" ) );
    }
    else if ( !geometry->deleteVertex( error->vidx() ) )
    {
      error->setFixFailed( tr( "Failed to delete vertex" ) );
    }
    else
    {
      changes[error->layerId()][error->featureId()].append( Change( ChangeNode, ChangeRemoved, vidx ) );
      // Avoid duplicate nodes as result of deleting spike vertex
      if ( QgsGeometryUtils::sqrDistance2D( p1, p3 ) < mContext->tolerance &&
           QgsGeometryCheckerUtils::canDeleteVertex( geometry, vidx.part, vidx.ring ) &&
           geometry->deleteVertex( error->vidx() ) ) // error->vidx points to p3 after removing p2
      {
        changes[error->layerId()][error->featureId()].append( Change( ChangeNode, ChangeRemoved, QgsVertexId( vidx.part, vidx.ring, ( vidx.vertex + 1 ) % n ) ) );
      }
      feature.setGeometry( featureGeometry );
      featurePool->updateFeature( feature );
      error->setFixed( method );
    }
  }
  else
  {
    error->setFixFailed( tr( "Unknown method" ) );
  }
}

QStringList QgsGeometryAngleCheck::getResolutionMethods() const
{
  static QStringList methods = QStringList() << tr( "Delete node with small angle" ) << tr( "No action" );
  return methods;
}
