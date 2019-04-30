#ifndef STRESS_DIVERGENCE
#define STRESS_DIVERGENCE

#include "plato/SimplexMechanics.hpp"

namespace Plato
{

/******************************************************************************/
/*! Stress Divergence functor.
  
    Given a stress, compute the stress divergence.
*/
/******************************************************************************/
template<int SpaceDim, int NumDofsPerNode=SpaceDim, int DofOffset=0>
class StressDivergence : public Plato::SimplexMechanics<SpaceDim>
{
  private:

    using Plato::SimplexMechanics<SpaceDim>::m_numNodesPerCell;

    Plato::OrdinalType m_voigt[SpaceDim][SpaceDim];

  public:

    StressDivergence()
    {
      Plato::OrdinalType voigtTerm=0;
      for(Plato::OrdinalType iDof=0; iDof<SpaceDim; iDof++){
        m_voigt[iDof][iDof] = voigtTerm++;
      }
      for (Plato::OrdinalType jDof=SpaceDim-1; jDof>=1; jDof--){
        for (Plato::OrdinalType iDof=jDof-1; iDof>=0; iDof--){
          m_voigt[iDof][jDof] = voigtTerm;
          m_voigt[jDof][iDof] = voigtTerm++;
        }
      }
    }

    template<
      typename ForcingScalarType, 
      typename StressScalarType,
      typename GradientScalarType,
      typename VolumeScalarType>
    DEVICE_TYPE inline void
    operator()( Plato::OrdinalType cellOrdinal,
                Plato::ScalarMultiVectorT< ForcingScalarType  > forcing,
                Plato::ScalarMultiVectorT< StressScalarType   > stress,
                Plato::ScalarArray3DT<     GradientScalarType > gradient,
                Plato::ScalarVectorT<      VolumeScalarType   > cellVolume ) const {


      for(Plato::OrdinalType iDim=0; iDim<SpaceDim; iDim++){
        for( Plato::OrdinalType iNode=0; iNode<m_numNodesPerCell; iNode++){
          Plato::OrdinalType localOrdinal = iNode*NumDofsPerNode+iDim+DofOffset;
          forcing(cellOrdinal, localOrdinal) = 0.0;
          for(Plato::OrdinalType jDim=0; jDim<SpaceDim; jDim++){
            forcing(cellOrdinal,localOrdinal) += 
              cellVolume(cellOrdinal)*stress(cellOrdinal,m_voigt[iDim][jDim])*gradient(cellOrdinal,iNode,jDim);
          }
        }
      }
    }
};

} // namespace Plato

#endif
