import pytest
from pydantic import ValidationError

from app.device_configuration import ConfigurationSnapshot, active_thresholds, save_configuration_query
from app.models import DeviceConfiguration


def snapshot(zones):
    return ConfigurationSnapshot.model_validate({"type":"config.snapshot", "deviceId":"garden", "revision":2, "config":{"zones":zones}})


def test_only_reported_thresholds_are_retained_and_stale_revisions_are_hidden():
    message = snapshot([{"id":1, "startWateringPercent":27, "stopWateringPercent":62, "extra":"ignored"}])
    params = save_configuration_query(message).compile().params
    assert params["zones"] == [{"id":1, "startWateringPercent":27, "stopWateringPercent":62}]
    configuration = DeviceConfiguration(device_id="garden", revision=2, zones=params["zones"])
    assert active_thresholds(configuration, 2) == {1:{"startWateringPercent":27, "stopWateringPercent":62}}
    assert active_thresholds(configuration, 3) == {}
    assert active_thresholds(None, 2) == {}


@pytest.mark.parametrize("zones", [
    [], [{"id":1, "startWateringPercent":70, "stopWateringPercent":60}],
    [{"id":1, "startWateringPercent":-1, "stopWateringPercent":60}],
    [{"id":1, "startWateringPercent":20, "stopWateringPercent":101}],
    [{"id":1, "startWateringPercent":20, "stopWateringPercent":60}] * 2,
])
def test_invalid_configuration_does_not_supply_thresholds(zones):
    with pytest.raises(ValidationError):
        snapshot(zones)
