#ifndef STABILIZED_ELASTOSTATIC_RESIDUAL_HPP
#define STABILIZED_ELASTOSTATIC_RESIDUAL_HPP

#include <memory>

#include "plato/PlatoTypes.hpp"
#include "plato/SimplexFadTypes.hpp"
#include "plato/SimplexMechanics.hpp"
#include "plato/Kinematics.hpp"
#include "plato/Kinetics.hpp"
#include "plato/FluxDivergence.hpp"
#include "plato/StressDivergence.hpp"
#include "plato/PressureDivergence.hpp"
#include "plato/ProjectToNode.hpp"
#include "plato/Projection.hpp"
#include "plato/AbstractVectorFunctionVMS.hpp"
#include "plato/ApplyWeighting.hpp"
#include "plato/CellForcing.hpp"
#include "plato/InterpolateFromNodal.hpp"
#include "plato/LinearTetCubRuleDegreeOne.hpp"
#include "plato/LinearElasticMaterial.hpp"
#include "plato/NaturalBCs.hpp"
#include "plato/BodyLoads.hpp"

namespace Plato
{

/******************************************************************************/
template<typename EvaluationType, typename IndicatorFunctionType>
class StabilizedElastostaticResidual :
        public Plato::SimplexStabilizedMechanics<EvaluationType::SpatialDim>,
        public Plato::AbstractVectorFunctionVMS<EvaluationType>
/******************************************************************************/
{
private:
    static constexpr int SpaceDim = EvaluationType::SpatialDim;

    static constexpr int NMechDims  = SpaceDim;
    static constexpr int NPressDims = 1;

    static constexpr int MDofOffset = 0;
    static constexpr int PDofOffset = SpaceDim;

    using Plato::SimplexStabilizedMechanics<SpaceDim>::mNumVoigtTerms;
    using Plato::Simplex<SpaceDim>::mNumNodesPerCell;
    using Plato::SimplexStabilizedMechanics<SpaceDim>::mNumDofsPerNode;
    using Plato::SimplexStabilizedMechanics<SpaceDim>::mNumDofsPerCell;

    using Plato::AbstractVectorFunctionVMS<EvaluationType>::mMesh;
    using Plato::AbstractVectorFunctionVMS<EvaluationType>::mDataMap;
    using Plato::AbstractVectorFunctionVMS<EvaluationType>::mMeshSets;

    using StateScalarType     = typename EvaluationType::StateScalarType;
    using NodeStateScalarType = typename EvaluationType::NodeStateScalarType;
    using ControlScalarType   = typename EvaluationType::ControlScalarType;
    using ConfigScalarType    = typename EvaluationType::ConfigScalarType;
    using ResultScalarType    = typename EvaluationType::ResultScalarType;

    IndicatorFunctionType mIndicatorFunction;
    Plato::ApplyWeighting<SpaceDim, mNumVoigtTerms, IndicatorFunctionType> mApplyTensorWeighting;
    Plato::ApplyWeighting<SpaceDim, SpaceDim,       IndicatorFunctionType> mApplyVectorWeighting;
    Plato::ApplyWeighting<SpaceDim, 1,              IndicatorFunctionType> mApplyScalarWeighting;

    std::shared_ptr<Plato::BodyLoads<SpaceDim,mNumDofsPerNode>> mBodyLoads;

    std::shared_ptr<Plato::NaturalBCs<SpaceDim, NMechDims, mNumDofsPerNode, MDofOffset>> mBoundaryLoads;

    std::shared_ptr<Plato::LinearTetCubRuleDegreeOne<EvaluationType::SpatialDim>> mCubatureRule;

    Teuchos::RCP<Plato::LinearElasticMaterial<SpaceDim>> mMaterialModel;

    std::vector<std::string> mPlottable;

public:
    /**************************************************************************/
    StabilizedElastostaticResidual(Omega_h::Mesh& aMesh,
                               Omega_h::MeshSets& aMeshSets,
                               Plato::DataMap& aDataMap,
                               Teuchos::ParameterList& aProblemParams,
                               Teuchos::ParameterList& aPenaltyParams) :
            Plato::AbstractVectorFunctionVMS<EvaluationType>(aMesh, aMeshSets, aDataMap),
            mIndicatorFunction(aPenaltyParams),
            mApplyTensorWeighting(mIndicatorFunction),
            mApplyVectorWeighting(mIndicatorFunction),
            mApplyScalarWeighting(mIndicatorFunction),
            mBodyLoads(nullptr),
            mBoundaryLoads(nullptr),
            mCubatureRule(std::make_shared<Plato::LinearTetCubRuleDegreeOne<EvaluationType::SpatialDim>>())
    /**************************************************************************/
    {
        // create material model and get stiffness
        //
        Plato::ElasticModelFactory<SpaceDim> mmfactory(aProblemParams);
        mMaterialModel = mmfactory.create();
  

        // parse body loads
        // 
        if(aProblemParams.isSublist("Body Loads"))
        {
            mBodyLoads = std::make_shared<Plato::BodyLoads<SpaceDim,mNumDofsPerNode>>(aProblemParams.sublist("Body Loads"));
        }
  
        // parse mechanical boundary Conditions
        // 
        if(aProblemParams.isSublist("Mechanical Natural Boundary Conditions"))
        {
            mBoundaryLoads = std::make_shared<Plato::NaturalBCs<SpaceDim, NMechDims, mNumDofsPerNode, MDofOffset>>
                                (aProblemParams.sublist("Mechanical Natural Boundary Conditions"));
        }
  
        auto tResidualParams = aProblemParams.sublist("Elliptic");
        if( tResidualParams.isType<Teuchos::Array<std::string>>("Plottable") )
          mPlottable = tResidualParams.get<Teuchos::Array<std::string>>("Plottable").toVector();

    }

    /**************************************************************************/
    void evaluate(const Plato::ScalarMultiVectorT<StateScalarType>     & aStateWS,
                  const Plato::ScalarMultiVectorT<NodeStateScalarType> & aPGradWS,
                  const Plato::ScalarMultiVectorT<ControlScalarType>   & aControlWS,
                  const Plato::ScalarArray3DT<ConfigScalarType>        & aConfigWS,
                  Plato::ScalarMultiVectorT<ResultScalarType>          & aResultWS,
                  Plato::Scalar aTimeStep = 0.0) const
    /**************************************************************************/
    {
      auto tNumCells = mMesh.nelems();

      using GradScalarType =
      typename Plato::fad_type_t<Plato::SimplexStabilizedMechanics
                <EvaluationType::SpatialDim>, StateScalarType, ConfigScalarType>;

      Plato::ComputeGradientWorkset <SpaceDim> computeGradient;
      Plato::StabilizedKinematics   <SpaceDim> kinematics;
      Plato::StabilizedKinetics     <SpaceDim> kinetics(mMaterialModel);

      Plato::InterpolateFromNodal   <SpaceDim, SpaceDim, 0, SpaceDim>         interpolatePGradFromNodal;
      Plato::InterpolateFromNodal   <SpaceDim, mNumDofsPerNode, PDofOffset>  interpolatePressureFromNodal;
      
      Plato::FluxDivergence         <SpaceDim, mNumDofsPerNode, PDofOffset> stabDivergence;
      Plato::StressDivergence       <SpaceDim, mNumDofsPerNode, MDofOffset> stressDivergence;
      Plato::PressureDivergence     <SpaceDim, mNumDofsPerNode>             pressureDivergence;

      Plato::ProjectToNode          <SpaceDim, mNumDofsPerNode, PDofOffset> projectVolumeStrain;

      Plato::ScalarVectorT      <ResultScalarType>    tVolStrain      ("volume strain",      tNumCells);
      Plato::ScalarVectorT      <ResultScalarType>    tPressure       ("GP pressure",        tNumCells);
      Plato::ScalarVectorT      <ConfigScalarType>    tCellVolume     ("cell weight",        tNumCells);
      Plato::ScalarMultiVectorT <NodeStateScalarType> tProjectedPGrad ("projected p grad",   tNumCells, SpaceDim);
      Plato::ScalarMultiVectorT <ResultScalarType>    tCellStab       ("cell stabilization", tNumCells, SpaceDim);
      Plato::ScalarMultiVectorT <GradScalarType>      tPGrad          ("pressure grad",      tNumCells, SpaceDim);
      Plato::ScalarMultiVectorT <ResultScalarType>    tDevStress      ("deviatoric stress",  tNumCells, mNumVoigtTerms);
      Plato::ScalarMultiVectorT <GradScalarType>      tDGrad          ("displacement grad",  tNumCells, mNumVoigtTerms);
      Plato::ScalarArray3DT     <ConfigScalarType>    tGradient       ("gradient",           tNumCells, mNumNodesPerCell, SpaceDim);

      auto tQuadratureWeight = mCubatureRule->getCubWeight();
      auto tBasisFunctions   = mCubatureRule->getBasisFunctions();

      auto& applyTensorWeighting = mApplyTensorWeighting;
      auto& applyVectorWeighting = mApplyVectorWeighting;
      auto& applyScalarWeighting = mApplyScalarWeighting;

      Kokkos::parallel_for(Kokkos::RangePolicy<int>(0,tNumCells), LAMBDA_EXPRESSION(int cellOrdinal)
      {
        // compute gradient operator and cell volume
        //
        computeGradient(cellOrdinal, tGradient, aConfigWS, tCellVolume);
        tCellVolume(cellOrdinal) *= tQuadratureWeight;

        // compute symmetric gradient of displacement, pressure gradient, and temperature gradient
        //
        kinematics(cellOrdinal, tDGrad, tPGrad, aStateWS, tGradient);

        // interpolate projected PGrad, pressure, and temperature to gauss point
        //
        interpolatePGradFromNodal        ( cellOrdinal, tBasisFunctions, aPGradWS, tProjectedPGrad );
        interpolatePressureFromNodal     ( cellOrdinal, tBasisFunctions, aStateWS, tPressure       );

        // compute the constitutive response
        //
        kinetics(cellOrdinal,     tCellVolume,
                 tProjectedPGrad, tPressure,
                 tDGrad,          tPGrad,
                 tDevStress,      tVolStrain,  tCellStab);

        // apply weighting
        //
        applyTensorWeighting (cellOrdinal, tDevStress, aControlWS);
        applyVectorWeighting (cellOrdinal, tCellStab,  aControlWS);
        applyScalarWeighting (cellOrdinal, tPressure,  aControlWS);
        applyScalarWeighting (cellOrdinal, tVolStrain, aControlWS);
    
        // compute divergence
        //
        stressDivergence    (cellOrdinal, aResultWS,  tDevStress, tGradient, tCellVolume);
        pressureDivergence  (cellOrdinal, aResultWS,  tPressure,  tGradient, tCellVolume);
        stabDivergence      (cellOrdinal, aResultWS,  tCellStab,  tGradient, tCellVolume, -1.0);

        projectVolumeStrain (cellOrdinal, tCellVolume, tBasisFunctions, tVolStrain, aResultWS);

      }, "Cauchy stress");

      if( mBodyLoads != nullptr )
      {
          mBodyLoads->get( mMesh, aStateWS, aControlWS, aResultWS );
      }

      if( mBoundaryLoads != nullptr )
      {
          mBoundaryLoads->get( &mMesh, mMeshSets, aStateWS, aControlWS, aResultWS );
      }

      if( std::count(m_plottable.begin(),m_plottable.end(), "pressure"          ) ) toMap(m_dataMap, tPressure, "pressure");
      if( std::count(m_plottable.begin(),m_plottable.end(), "deviatoric stress" ) ) toMap(m_dataMap, tDevStress, "deviatoric stress");

    }
};
// class ElastostaticResidual

} // namespace Plato
#endif
