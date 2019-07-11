#ifndef LINEARTHERMOELASTICMATERIAL_HPP
#define LINEARTHERMOELASTICMATERIAL_HPP

#include <Omega_h_matrix.hpp>
#include <Teuchos_ParameterList.hpp>

#include "plato/PlatoStaticsTypes.hpp"

namespace Plato {

/******************************************************************************/
/*!
  \brief Base class for Linear Thermoelastic material models
*/
  template<int SpatialDim>
  class LinearThermoelasticMaterial
/******************************************************************************/
{
  protected:
    static constexpr auto m_numVoigtTerms = (SpatialDim == 3) ? 6 : 
                                           ((SpatialDim == 2) ? 3 :
                                          (((SpatialDim == 1) ? 1 : 0)));
    static_assert(m_numVoigtTerms, "SpatialDim must be 1, 2, or 3.");

    Plato::Scalar m_cellDensity;
    Plato::Scalar m_cellSpecificHeat;
    Omega_h::Matrix<m_numVoigtTerms,m_numVoigtTerms> m_cellStiffness;
    Plato::Scalar m_cellThermalExpansionCoef;
    Omega_h::Matrix<SpatialDim, SpatialDim> m_cellThermalConductivity;
    Plato::Scalar m_cellReferenceTemperature;

    Plato::Scalar m_temperatureScaling;
    Plato::Scalar m_pressureScaling;

  public:
    LinearThermoelasticMaterial();
    decltype(m_cellDensity)               getMassDensity()          const {return m_cellDensity;}
    decltype(m_cellSpecificHeat)          getSpecificHeat()         const {return m_cellSpecificHeat;}
    decltype(m_cellStiffness)             getStiffnessMatrix()      const {return m_cellStiffness;}
    decltype(m_cellThermalExpansionCoef)  getThermalExpansion()     const {return m_cellThermalExpansionCoef;}
    decltype(m_cellThermalConductivity)   getThermalConductivity()  const {return m_cellThermalConductivity;}
    decltype(m_cellReferenceTemperature)  getReferenceTemperature() const {return m_cellReferenceTemperature;}
    decltype(m_temperatureScaling)        getTemperatureScaling()   const {return m_temperatureScaling;}
    decltype(m_pressureScaling)           getPressureScaling()      const {return m_pressureScaling;}
};

/******************************************************************************/
template<int SpatialDim>
LinearThermoelasticMaterial<SpatialDim>::
LinearThermoelasticMaterial()
/******************************************************************************/
{
  for(int i=0; i<m_numVoigtTerms; i++)
    for(int j=0; j<m_numVoigtTerms; j++)
      m_cellStiffness(i,j) = 0.0;

  m_cellThermalExpansionCoef = 0.0;

  for(int i=0; i<SpatialDim; i++)
    for(int j=0; j<SpatialDim; j++)
      m_cellThermalConductivity(i,j) = 0.0;

  m_cellReferenceTemperature = 0.0;

  m_temperatureScaling = 1.0;
  m_pressureScaling = 1.0;
}

/******************************************************************************/
/*!
  \brief Derived class for isotropic linear thermoelastic material model
*/
  template<int SpatialDim>
  class IsotropicLinearThermoelasticMaterial : public LinearThermoelasticMaterial<SpatialDim>
/******************************************************************************/
{
  public:
    IsotropicLinearThermoelasticMaterial(const Teuchos::ParameterList& paramList);
    virtual ~IsotropicLinearThermoelasticMaterial(){}
};
// class IsotropicLinearThermoelasticMaterial

/******************************************************************************/
/*!
  \brief Factory for creating material models
*/
  template<int SpatialDim>
  class ThermoelasticModelFactory
/******************************************************************************/
{
  public:
    ThermoelasticModelFactory(const Teuchos::ParameterList& paramList) : m_paramList(paramList) {}
    Teuchos::RCP<Plato::LinearThermoelasticMaterial<SpatialDim>> create();
  private:
    const Teuchos::ParameterList& m_paramList;
};
/******************************************************************************/
template<int SpatialDim>
Teuchos::RCP<LinearThermoelasticMaterial<SpatialDim>>
ThermoelasticModelFactory<SpatialDim>::create()
/******************************************************************************/
{
  auto modelParamList = m_paramList.get<Teuchos::ParameterList>("Material Model");

  if( modelParamList.isSublist("Isotropic Linear Thermoelastic") ){
    return Teuchos::rcp(new Plato::IsotropicLinearThermoelasticMaterial<SpatialDim>(modelParamList.sublist("Isotropic Linear Thermoelastic")));
  }
  return Teuchos::RCP<Plato::LinearThermoelasticMaterial<SpatialDim>>(nullptr);
}

} // namespace Plato

#endif
