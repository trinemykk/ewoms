/*
  Copyright (C) 2011-2013 by Andreas Lauser

  This file is part of the Open Porous Media project (OPM).

  OPM is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 2 of the License, or
  (at your option) any later version.

  OPM is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with OPM.  If not, see <http://www.gnu.org/licenses/>.
*/
/*!
 * \file
 * \copydoc Ewoms::VtkBlackOilModule
 */
#ifndef EWOMS_VTK_BLACK_OIL_MODULE_HH
#define EWOMS_VTK_BLACK_OIL_MODULE_HH

#include "vtkmultiwriter.hh"
#include "baseoutputmodule.hh"

#include <opm/core/utility/PropertySystem.hpp>
#include <ewoms/common/parametersystem.hh>

#include <dune/common/fvector.hh>

#include <cstdio>

namespace Opm {
namespace Properties {
// create new type tag for the VTK multi-phase output
NEW_TYPE_TAG(VtkBlackOil);

// create the property tags needed for the multi phase module
NEW_PROP_TAG(VtkWriteGasDissolutionFactor);
NEW_PROP_TAG(VtkWriteSaturatedOilGasDissolutionFactor);
NEW_PROP_TAG(VtkWriteGasFormationFactor);
NEW_PROP_TAG(VtkWriteOilFormationFactor);
NEW_PROP_TAG(VtkWriteOilSaturationPressure);
NEW_PROP_TAG(VtkOutputFormat);

// set default values for what quantities to output
SET_BOOL_PROP(VtkBlackOil, VtkWriteGasDissolutionFactor, false);
SET_BOOL_PROP(VtkBlackOil, VtkWriteSaturatedOilGasDissolutionFactor, false);
SET_BOOL_PROP(VtkBlackOil, VtkWriteGasFormationFactor, false);
SET_BOOL_PROP(VtkBlackOil, VtkWriteOilFormationFactor, false);
SET_BOOL_PROP(VtkBlackOil, VtkWriteOilSaturationPressure, false);
} // namespace Properties
} // namespace Opm

namespace Ewoms {
/*!
 * \ingroup Vtk
 *
 * \brief VTK output module for the black oil model's parameters.
 */
template <class TypeTag>
class VtkBlackOilModule : public BaseOutputModule<TypeTag>
{
    typedef BaseOutputModule<TypeTag> ParentType;

    typedef typename GET_PROP_TYPE(TypeTag, Simulator) Simulator;
    typedef typename GET_PROP_TYPE(TypeTag, Scalar) Scalar;
    typedef typename GET_PROP_TYPE(TypeTag, ElementContext) ElementContext;

    typedef typename GET_PROP_TYPE(TypeTag, GridView) GridView;
    typedef typename GET_PROP_TYPE(TypeTag, FluidSystem) FluidSystem;

    static const int vtkFormat = GET_PROP_VALUE(TypeTag, VtkOutputFormat);
    typedef Ewoms::VtkMultiWriter<GridView, vtkFormat> VtkMultiWriter;

    enum { oilPhaseIdx = FluidSystem::oilPhaseIdx };
    enum { gasPhaseIdx = FluidSystem::gasPhaseIdx };
    enum { gasCompIdx = FluidSystem::gasCompIdx };

    typedef typename ParentType::ScalarBuffer ScalarBuffer;

public:
    VtkBlackOilModule(const Simulator &simulator)
        : ParentType(simulator)
    { }

    /*!
     * \brief Register all run-time parameters for the multi-phase VTK output
     * module.
     */
    static void registerParameters()
    {
        EWOMS_REGISTER_PARAM(TypeTag, bool, VtkWriteGasDissolutionFactor,
                             "Include the gas dissolution factor (R_s) of the observed oil "
                             "in the VTK output files");
        EWOMS_REGISTER_PARAM(TypeTag, bool, VtkWriteSaturatedOilGasDissolutionFactor,
                             "Include the gas dissolution factor (R_s,sat) of gas saturated "
                             "oil in the VTK output files");
        EWOMS_REGISTER_PARAM(TypeTag, bool, VtkWriteGasFormationFactor,
                             "Include the gas formation volume factor (B_g) in the "
                             "VTK output files");
        EWOMS_REGISTER_PARAM(TypeTag, bool, VtkWriteOilFormationFactor,
                             "Include the oil formation volume factor (B_o) of gas saturated "
                             "oil in the VTK output files");
        EWOMS_REGISTER_PARAM(TypeTag, bool, VtkWriteOilSaturationPressure,
                             "Include the saturation pressure of oil in the "
                             "VTK output files");
    }

    /*!
     * \brief Allocate memory for the scalar fields we would like to
     *        write to the VTK file.
     */
    void allocBuffers()
    {
        if (gasDissolutionFactorOutput_())
            this->resizeScalarBuffer_(gasDissolutionFactor_);
        if (saturatedOilGasDissolutionFactorOutput_())
            this->resizeScalarBuffer_(saturatedOilGasDissolutionFactor_);
        if (gasFormationFactorOutput_())
            this->resizeScalarBuffer_(gasFormationFactor_);
        if (saturatedOilFormationFactorOutput_())
            this->resizeScalarBuffer_(saturatedOilFormationFactor_);
        if (oilSaturationPressureOutput_())
            this->resizeScalarBuffer_(oilSaturationPressure_);
    }

    /*!
     * \brief Modify the internal buffers according to the intensive quantities relevant for
     *        an element
     */
    void processElement(const ElementContext &elemCtx)
    {
        for (int dofIdx = 0; dofIdx < elemCtx.numPrimaryDof(/*timeIdx=*/0); ++dofIdx) {
            const auto &fs = elemCtx.intensiveQuantities(dofIdx, /*timeIdx=*/0).fluidState();
            int globalDofIdx = elemCtx.globalSpaceIndex(dofIdx, /*timeIdx=*/0);
            Scalar po = fs.pressure(oilPhaseIdx);
            Scalar X_oG = fs.massFraction(oilPhaseIdx, gasCompIdx);
            int regionIdx = elemCtx.primaryVars(dofIdx, /*timeIdx=*/0).pvtRegionIndex();
            Scalar rhooRef = FluidSystem::referenceDensity(oilPhaseIdx, regionIdx);
            Scalar rhogRef = FluidSystem::referenceDensity(gasPhaseIdx, regionIdx);

            if (gasDissolutionFactorOutput_())
                gasDissolutionFactor_[globalDofIdx] = X_oG / rhogRef * rhooRef / (1 - X_oG);
            if (saturatedOilGasDissolutionFactorOutput_())
                saturatedOilGasDissolutionFactor_[globalDofIdx] =
                    FluidSystem::gasDissolutionFactor(po, regionIdx);
            if (gasFormationFactorOutput_())
                gasFormationFactor_[globalDofIdx] =
                    FluidSystem::gasFormationFactor(po, regionIdx);
            if (saturatedOilFormationFactorOutput_())
                saturatedOilFormationFactor_[globalDofIdx] =
                    FluidSystem::saturatedOilFormationFactor(po, regionIdx);
            if (oilSaturationPressureOutput_())
                oilSaturationPressure_[globalDofIdx] =
                    FluidSystem::oilSaturationPressure(X_oG, regionIdx);
        }
    }

    /*!
     * \brief Add all buffers to the VTK output writer.
     */
    void commitBuffers(BaseOutputWriter &baseWriter)
    {
        VtkMultiWriter *vtkWriter = dynamic_cast<VtkMultiWriter*>(&baseWriter);
        if (!vtkWriter)
            return;

        if (gasDissolutionFactorOutput_())
            this->commitScalarBuffer_(baseWriter, "R_s", gasDissolutionFactor_);
        if (saturatedOilGasDissolutionFactorOutput_())
            this->commitScalarBuffer_(baseWriter, "R_s,sat", saturatedOilGasDissolutionFactor_);
        if (gasFormationFactorOutput_())
            this->commitScalarBuffer_(baseWriter, "B_g", gasFormationFactor_);
        if (saturatedOilFormationFactorOutput_())
            this->commitScalarBuffer_(baseWriter, "B_o", saturatedOilFormationFactor_);
        if (oilSaturationPressureOutput_())
            this->commitScalarBuffer_(baseWriter, "pressure_sat,o", oilSaturationPressure_);
    }

private:
    static bool gasDissolutionFactorOutput_()
    { return EWOMS_GET_PARAM(TypeTag, bool, VtkWriteGasDissolutionFactor); }

    static bool saturatedOilGasDissolutionFactorOutput_()
    { return EWOMS_GET_PARAM(TypeTag, bool, VtkWriteSaturatedOilGasDissolutionFactor); }

    static bool gasFormationFactorOutput_()
    { return EWOMS_GET_PARAM(TypeTag, bool, VtkWriteGasFormationFactor); }

    static bool saturatedOilFormationFactorOutput_()
    { return EWOMS_GET_PARAM(TypeTag, bool, VtkWriteOilFormationFactor); }

    static bool oilSaturationPressureOutput_()
    { return EWOMS_GET_PARAM(TypeTag, bool, VtkWriteOilSaturationPressure); }

    ScalarBuffer gasDissolutionFactor_;
    ScalarBuffer saturatedOilGasDissolutionFactor_;
    ScalarBuffer gasFormationFactor_;
    ScalarBuffer saturatedOilFormationFactor_;
    ScalarBuffer oilSaturationPressure_;
};
} // namespace Ewoms

#endif
