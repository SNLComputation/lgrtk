#ifndef INTERNAL_THERMOELASTIC_ENERGY_HPP
#define INTERNAL_THERMOELASTIC_ENERGY_HPP

#include "plato/SimplexFadTypes.hpp"
#include "plato/SimplexThermomechanics.hpp"
#include "plato/ScalarProduct.hpp"
#include "plato/ApplyWeighting.hpp"
#include "plato/TMKinematics.hpp"
#include "plato/TMKinetics.hpp"
#include "plato/ImplicitFunctors.hpp"
#include "plato/InterpolateFromNodal.hpp"
#include "plato/AbstractScalarFunction.hpp"
#include "plato/LinearTetCubRuleDegreeOne.hpp"
#include "plato/LinearThermoelasticMaterial.hpp"
#include "plato/ToMap.hpp"
#include "plato/ExpInstMacros.hpp"
#include "plato/Simp.hpp"
#include "plato/Ramp.hpp"
#include "plato/Heaviside.hpp"

/******************************************************************************/
template<typename EvaluationType, typename IndicatorFunctionType>
class InternalThermoelasticEnergy : 
  public Plato::SimplexThermomechanics<EvaluationType::SpatialDim>,
  public AbstractScalarFunction<EvaluationType>
/******************************************************************************/
{
  private:
    static constexpr int SpaceDim = EvaluationType::SpatialDim;
    static constexpr int TDofOffset = SpaceDim;
    
    using Plato::SimplexThermomechanics<SpaceDim>::m_numVoigtTerms;
    using Simplex<SpaceDim>::m_numNodesPerCell;
    using Plato::SimplexThermomechanics<SpaceDim>::m_numDofsPerCell;
    using Plato::SimplexThermomechanics<SpaceDim>::m_numDofsPerNode;

    using AbstractScalarFunction<EvaluationType>::mMesh;
    using AbstractScalarFunction<EvaluationType>::m_dataMap;

    using StateScalarType   = typename EvaluationType::StateScalarType;
    using ControlScalarType = typename EvaluationType::ControlScalarType;
    using ConfigScalarType  = typename EvaluationType::ConfigScalarType;
    using ResultScalarType  = typename EvaluationType::ResultScalarType;

    Teuchos::RCP<Plato::LinearThermoelasticMaterial<SpaceDim>> m_materialModel;
    
    Plato::Scalar m_quadratureWeight;

    IndicatorFunctionType m_indicatorFunction;
    ApplyWeighting<SpaceDim, m_numVoigtTerms, IndicatorFunctionType> m_applyStressWeighting;
    ApplyWeighting<SpaceDim, SpaceDim,        IndicatorFunctionType> m_applyFluxWeighting;

    std::shared_ptr<Plato::LinearTetCubRuleDegreeOne<EvaluationType::SpatialDim>> m_CubatureRule;

    std::vector<std::string> m_plottable;

  public:
    /**************************************************************************/
    InternalThermoelasticEnergy(Omega_h::Mesh& aMesh,
                          Omega_h::MeshSets& aMeshSets,
                          Plato::DataMap& aDataMap,
                          Teuchos::ParameterList& aProblemParams,
                          Teuchos::ParameterList& aPenaltyParams ) :
            AbstractScalarFunction<EvaluationType>(aMesh, aMeshSets, aDataMap, "Internal Thermoelastic Energy"),
            m_indicatorFunction(aPenaltyParams),
            m_applyStressWeighting(m_indicatorFunction),
            m_applyFluxWeighting(m_indicatorFunction),
            m_CubatureRule(std::make_shared<Plato::LinearTetCubRuleDegreeOne<EvaluationType::SpatialDim>>())
    /**************************************************************************/
    {
      Plato::ThermoelasticModelFactory<SpaceDim> mmfactory(aProblemParams);
      m_materialModel = mmfactory.create();

      if( aProblemParams.isType<Teuchos::Array<std::string>>("Plottable") )
        m_plottable = aProblemParams.get<Teuchos::Array<std::string>>("Plottable").toVector();
    }

    /**************************************************************************/
    void evaluate(const Plato::ScalarMultiVectorT<StateScalarType> & aState,
                  const Plato::ScalarMultiVectorT<ControlScalarType> & aControl,
                  const Plato::ScalarArray3DT<ConfigScalarType> & aConfig,
                  Plato::ScalarVectorT<ResultScalarType> & aResult,
                  Plato::Scalar aTimeStep = 0.0) const
    /**************************************************************************/
    {
      auto numCells = mMesh.nelems();

      using GradScalarType = 
        typename Plato::fad_type_t<Plato::SimplexThermomechanics<EvaluationType::SpatialDim>, StateScalarType, ConfigScalarType>;

      Plato::ComputeGradientWorkset<SpaceDim> computeGradient;
      TMKinematics<SpaceDim>                  kinematics;
      TMKinetics<SpaceDim>                    kinetics(m_materialModel);

      ScalarProduct<m_numVoigtTerms>          mechanicalScalarProduct;
      ScalarProduct<SpaceDim>                 thermalScalarProduct;

      Plato::InterpolateFromNodal<SpaceDim, m_numDofsPerNode, TDofOffset> interpolateFromNodal;

      Plato::ScalarVectorT<ConfigScalarType> cellVolume("cell weight",numCells);

      Plato::ScalarMultiVectorT<GradScalarType>   strain("strain", numCells, m_numVoigtTerms);
      Plato::ScalarMultiVectorT<GradScalarType>   tgrad ("tgrad",  numCells, SpaceDim);

      Plato::ScalarMultiVectorT<ResultScalarType> stress("stress", numCells, m_numVoigtTerms);
      Plato::ScalarMultiVectorT<ResultScalarType> flux  ("flux",   numCells, SpaceDim);

      Plato::ScalarArray3DT<ConfigScalarType>   gradient("gradient",numCells,m_numNodesPerCell,SpaceDim);

      Plato::ScalarVectorT<StateScalarType> temperature("Gauss point temperature", numCells);

      auto quadratureWeight = m_CubatureRule->getCubWeight();
      auto basisFunctions   = m_CubatureRule->getBasisFunctions();

      auto& applyStressWeighting = m_applyStressWeighting;
      auto& applyFluxWeighting   = m_applyFluxWeighting;
      Kokkos::parallel_for(Kokkos::RangePolicy<int>(0,numCells), LAMBDA_EXPRESSION(const int & aCellOrdinal)
      {
        computeGradient(aCellOrdinal, gradient, aConfig, cellVolume);
        cellVolume(aCellOrdinal) *= quadratureWeight;

        // compute strain and temperature gradient
        //
        kinematics(aCellOrdinal, strain, tgrad, aState, gradient);

        // compute stress and thermal flux
        //
        interpolateFromNodal(aCellOrdinal, basisFunctions, aState, temperature);
        kinetics(aCellOrdinal, stress, flux, strain, tgrad, temperature);

        // apply weighting
        //
        applyStressWeighting(aCellOrdinal, stress, aControl);
        applyFluxWeighting  (aCellOrdinal, flux,   aControl);
    
        // compute element internal energy (inner product of strain and weighted stress)
        //
        mechanicalScalarProduct(aCellOrdinal, aResult, stress, strain, cellVolume);
        thermalScalarProduct   (aCellOrdinal, aResult, flux,   tgrad,  cellVolume);

      },"energy gradient");
    }
};

#ifdef PLATO_1D
PLATO_EXPL_DEC(InternalThermoelasticEnergy, Plato::SimplexThermomechanics, 1)
#endif

#ifdef PLATO_2D
PLATO_EXPL_DEC(InternalThermoelasticEnergy, Plato::SimplexThermomechanics, 2)
#endif

#ifdef PLATO_3D
PLATO_EXPL_DEC(InternalThermoelasticEnergy, Plato::SimplexThermomechanics, 3)
#endif

#endif