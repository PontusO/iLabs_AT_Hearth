#include <app-common/zap-generated/callback.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <lib/support/Span.h>
#include <protocols/interaction_model/Constants.h>

using namespace chip;

// Cluster Init Functions
void emberAfClusterInitCallback(EndpointId endpoint, ClusterId clusterId)
{
    switch (clusterId)
    {
     case  app::Clusters::AccessControl::Id:
        emberAfAccessControlClusterInitCallback(endpoint);
        break;
     case  app::Clusters::AdministratorCommissioning::Id:
        emberAfAdministratorCommissioningClusterInitCallback(endpoint);
        break;
     case  app::Clusters::AirQuality::Id:
        emberAfAirQualityClusterInitCallback(endpoint);
        break;
     case  app::Clusters::BasicInformation::Id:
        emberAfBasicInformationClusterInitCallback(endpoint);
        break;
     case  app::Clusters::BooleanState::Id:
        emberAfBooleanStateClusterInitCallback(endpoint);
        break;
     case  app::Clusters::Chime::Id:
        emberAfChimeClusterInitCallback(endpoint);
        break;
     case  app::Clusters::ColorControl::Id:
        emberAfColorControlClusterInitCallback(endpoint);
        break;
     case  app::Clusters::Descriptor::Id:
        emberAfDescriptorClusterInitCallback(endpoint);
        break;
     case  app::Clusters::DeviceEnergyManagement::Id:
        emberAfDeviceEnergyManagementClusterInitCallback(endpoint);
        break;
     case  app::Clusters::DeviceEnergyManagementMode::Id:
        emberAfDeviceEnergyManagementModeClusterInitCallback(endpoint);
        break;
     case  app::Clusters::DoorLock::Id:
        emberAfDoorLockClusterInitCallback(endpoint);
        break;
     case  app::Clusters::ElectricalEnergyMeasurement::Id:
        emberAfElectricalEnergyMeasurementClusterInitCallback(endpoint);
        break;
     case  app::Clusters::ElectricalPowerMeasurement::Id:
        emberAfElectricalPowerMeasurementClusterInitCallback(endpoint);
        break;
     case  app::Clusters::EnergyEvse::Id:
        emberAfEnergyEvseClusterInitCallback(endpoint);
        break;
     case  app::Clusters::EnergyEvseMode::Id:
        emberAfEnergyEvseModeClusterInitCallback(endpoint);
        break;
     case  app::Clusters::FanControl::Id:
        emberAfFanControlClusterInitCallback(endpoint);
        break;
     case  app::Clusters::FlowMeasurement::Id:
        emberAfFlowMeasurementClusterInitCallback(endpoint);
        break;
     case  app::Clusters::GeneralCommissioning::Id:
        emberAfGeneralCommissioningClusterInitCallback(endpoint);
        break;
     case  app::Clusters::GeneralDiagnostics::Id:
        emberAfGeneralDiagnosticsClusterInitCallback(endpoint);
        break;
     case  app::Clusters::GroupKeyManagement::Id:
        emberAfGroupKeyManagementClusterInitCallback(endpoint);
        break;
     case  app::Clusters::Groups::Id:
        emberAfGroupsClusterInitCallback(endpoint);
        break;
     case  app::Clusters::Identify::Id:
        emberAfIdentifyClusterInitCallback(endpoint);
        break;
     case  app::Clusters::IlluminanceMeasurement::Id:
        emberAfIlluminanceMeasurementClusterInitCallback(endpoint);
        break;
     case  app::Clusters::LevelControl::Id:
        emberAfLevelControlClusterInitCallback(endpoint);
        break;
     case  app::Clusters::MeterIdentification::Id:
        emberAfMeterIdentificationClusterInitCallback(endpoint);
        break;
     case  app::Clusters::MicrowaveOvenControl::Id:
        emberAfMicrowaveOvenControlClusterInitCallback(endpoint);
        break;
     case  app::Clusters::MicrowaveOvenMode::Id:
        emberAfMicrowaveOvenModeClusterInitCallback(endpoint);
        break;
     case  app::Clusters::ModeSelect::Id:
        emberAfModeSelectClusterInitCallback(endpoint);
        break;
     case  app::Clusters::NetworkCommissioning::Id:
        emberAfNetworkCommissioningClusterInitCallback(endpoint);
        break;
     case  app::Clusters::OccupancySensing::Id:
        emberAfOccupancySensingClusterInitCallback(endpoint);
        break;
     case  app::Clusters::OnOff::Id:
        emberAfOnOffClusterInitCallback(endpoint);
        break;
     case  app::Clusters::OperationalCredentials::Id:
        emberAfOperationalCredentialsClusterInitCallback(endpoint);
        break;
     case  app::Clusters::OperationalState::Id:
        emberAfOperationalStateClusterInitCallback(endpoint);
        break;
     case  app::Clusters::OvenCavityOperationalState::Id:
        emberAfOvenCavityOperationalStateClusterInitCallback(endpoint);
        break;
     case  app::Clusters::OvenMode::Id:
        emberAfOvenModeClusterInitCallback(endpoint);
        break;
     case  app::Clusters::PowerSource::Id:
        emberAfPowerSourceClusterInitCallback(endpoint);
        break;
     case  app::Clusters::PowerTopology::Id:
        emberAfPowerTopologyClusterInitCallback(endpoint);
        break;
     case  app::Clusters::PressureMeasurement::Id:
        emberAfPressureMeasurementClusterInitCallback(endpoint);
        break;
     case  app::Clusters::PumpConfigurationAndControl::Id:
        emberAfPumpConfigurationAndControlClusterInitCallback(endpoint);
        break;
     case  app::Clusters::RvcCleanMode::Id:
        emberAfRvcCleanModeClusterInitCallback(endpoint);
        break;
     case  app::Clusters::RvcOperationalState::Id:
        emberAfRvcOperationalStateClusterInitCallback(endpoint);
        break;
     case  app::Clusters::RvcRunMode::Id:
        emberAfRvcRunModeClusterInitCallback(endpoint);
        break;
     case  app::Clusters::RefrigeratorAlarm::Id:
        emberAfRefrigeratorAlarmClusterInitCallback(endpoint);
        break;
     case  app::Clusters::RefrigeratorAndTemperatureControlledCabinetMode::Id:
        emberAfRefrigeratorAndTemperatureControlledCabinetModeClusterInitCallback(endpoint);
        break;
     case  app::Clusters::RelativeHumidityMeasurement::Id:
        emberAfRelativeHumidityMeasurementClusterInitCallback(endpoint);
        break;
     case  app::Clusters::SmokeCoAlarm::Id:
        emberAfSmokeCoAlarmClusterInitCallback(endpoint);
        break;
     case  app::Clusters::SoftwareDiagnostics::Id:
        emberAfSoftwareDiagnosticsClusterInitCallback(endpoint);
        break;
     case  app::Clusters::Switch::Id:
        emberAfSwitchClusterInitCallback(endpoint);
        break;
     case  app::Clusters::TemperatureControl::Id:
        emberAfTemperatureControlClusterInitCallback(endpoint);
        break;
     case  app::Clusters::TemperatureMeasurement::Id:
        emberAfTemperatureMeasurementClusterInitCallback(endpoint);
        break;
     case  app::Clusters::Thermostat::Id:
        emberAfThermostatClusterInitCallback(endpoint);
        break;
     case  app::Clusters::ThreadNetworkDiagnostics::Id:
        emberAfThreadNetworkDiagnosticsClusterInitCallback(endpoint);
        break;
     case  app::Clusters::ValveConfigurationAndControl::Id:
        emberAfValveConfigurationAndControlClusterInitCallback(endpoint);
        break;
     case  app::Clusters::WaterHeaterManagement::Id:
        emberAfWaterHeaterManagementClusterInitCallback(endpoint);
        break;
     case  app::Clusters::WaterHeaterMode::Id:
        emberAfWaterHeaterModeClusterInitCallback(endpoint);
        break;
     case  app::Clusters::WindowCovering::Id:
        emberAfWindowCoveringClusterInitCallback(endpoint);
        break;
    default:
        // Unrecognized cluster ID
        break;
    }
}
