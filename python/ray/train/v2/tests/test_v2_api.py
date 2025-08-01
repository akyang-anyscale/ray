import importlib
import sys

import pytest

import ray.cloudpickle as ray_pickle
import ray.train
from ray.train import FailureConfig, RunConfig, ScalingConfig
from ray.train.v2.api.data_parallel_trainer import DataParallelTrainer
from ray.train.v2.api.exceptions import ControllerError, WorkerGroupError


@pytest.mark.parametrize(
    "operation, raise_error",
    [
        (lambda: FailureConfig(fail_fast=True), True),
        (lambda: RunConfig(verbose=0), True),
        (lambda: FailureConfig(), False),
        (lambda: RunConfig(), False),
        (lambda: ScalingConfig(trainer_resources={"CPU": 1}), True),
        (lambda: ScalingConfig(), False),
    ],
)
def test_api_configs(operation, raise_error):
    if raise_error:
        with pytest.raises(DeprecationWarning):
            operation()
    else:
        try:
            operation()
        except Exception as e:
            pytest.fail(f"Default Operation raised an exception: {e}")


def test_run_config_default_failure_config():
    """Test that RunConfig creates a default FailureConfig from v2 API, not v1."""
    # Import the v2 FailureConfig and v1 FailureConfig for comparison
    from ray.train.v2.api.config import FailureConfig as FailureConfigV2

    # Create a RunConfig without specifying failure_config
    run_config = RunConfig()

    # Verify that the default failure_config is the v2 version
    assert run_config.failure_config is not None
    assert isinstance(run_config.failure_config, FailureConfigV2)
    assert type(run_config.failure_config) is FailureConfigV2

    # Verify that explicitly passing None also creates v2 FailureConfig
    run_config_explicit_none = RunConfig(failure_config=None)
    assert run_config_explicit_none.failure_config is not None
    assert isinstance(run_config_explicit_none.failure_config, FailureConfigV2)
    assert type(run_config_explicit_none.failure_config) is FailureConfigV2


def test_scaling_config_total_resources():
    """Test the patched scaling config total resources calculation."""
    num_workers = 2
    num_cpus_per_worker = 1
    num_gpus_per_worker = 1

    scaling_config = ScalingConfig(
        num_workers=num_workers,
        use_gpu=True,
        resources_per_worker={"CPU": num_cpus_per_worker, "GPU": num_gpus_per_worker},
    )
    scaling_config.total_resources == {
        "CPU": num_workers * num_cpus_per_worker,
        "GPU": num_workers * num_gpus_per_worker,
    }


def test_trainer_restore():
    with pytest.raises(DeprecationWarning):
        DataParallelTrainer.restore("dummy")

    with pytest.raises(DeprecationWarning):
        DataParallelTrainer.can_restore("dummy")


def test_serialized_imports(ray_start_4_cpus):
    """Check that captured imports are deserialized properly without circular imports."""

    from ray.train.lightgbm import LightGBMTrainer
    from ray.train.torch import TorchTrainer
    from ray.train.xgboost import XGBoostTrainer

    if sys.version_info < (3, 12):
        from ray.train.tensorflow import TensorflowTrainer
    else:
        TensorflowTrainer = None

    @ray.remote
    def dummy_task():
        _ = (TorchTrainer, TensorflowTrainer, XGBoostTrainer, LightGBMTrainer)

    ray.get(dummy_task.remote())


@pytest.mark.parametrize("env_v2_enabled", [True, False])
def test_train_v2_import(monkeypatch, env_v2_enabled):
    monkeypatch.setenv("RAY_TRAIN_V2_ENABLED", str(int(env_v2_enabled)))

    # Load from the public `ray.train` module
    # isort: off
    importlib.reload(ray.train)
    from ray.train import FailureConfig, Result, RunConfig

    # isort: on

    # Import from the absolute module paths as references
    from ray.train.v2.api.config import (
        FailureConfig as FailureConfigV2,
        RunConfig as RunConfigV2,
    )
    from ray.train.v2.api.result import Result as ResultV2

    if env_v2_enabled:
        assert RunConfig is RunConfigV2
        assert FailureConfig is FailureConfigV2
        assert Result is ResultV2
    else:
        assert RunConfig is not RunConfigV2
        assert FailureConfig is not FailureConfigV2
        assert Result is not ResultV2


@pytest.mark.parametrize(
    "error",
    [
        WorkerGroupError(
            "Training failed on multiple workers",
            {0: ValueError("worker 0 failed"), 1: RuntimeError("worker 1 failed")},
        ),
        ControllerError(Exception("Controller crashed")),
    ],
)
def test_exceptions_are_picklable(error):
    """Test that WorkerGroupError and ControllerError are picklable."""

    # Test pickle/unpickle for WorkerGroupError
    pickled_error = ray_pickle.dumps(error)
    unpickled_error = ray_pickle.loads(pickled_error)

    # Verify attributes are preserved
    assert str(unpickled_error) == str(error)
    assert type(unpickled_error) is type(error)


if __name__ == "__main__":
    import sys

    sys.exit(pytest.main(["-v", "-x", __file__]))
