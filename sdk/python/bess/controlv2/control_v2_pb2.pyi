from google.protobuf import any_pb2 as _any_pb2
from google.protobuf import descriptor_pb2 as _descriptor_pb2
from google.protobuf.internal import containers as _containers
from google.protobuf.internal import enum_type_wrapper as _enum_type_wrapper
from google.protobuf import descriptor as _descriptor
from google.protobuf import message as _message
from collections.abc import Iterable as _Iterable, Mapping as _Mapping
from typing import ClassVar as _ClassVar, Optional as _Optional, Union as _Union

DESCRIPTOR: _descriptor.FileDescriptor

class ObjectType(int, metaclass=_enum_type_wrapper.EnumTypeWrapper):
    __slots__ = ()
    OBJECT_TYPE_UNSPECIFIED: _ClassVar[ObjectType]
    PORT: _ClassVar[ObjectType]
    MODULE: _ClassVar[ObjectType]
    CONNECTION: _ClassVar[ObjectType]
    WORKER: _ClassVar[ObjectType]
    TRAFFIC_CLASS: _ClassVar[ObjectType]
    TASK: _ClassVar[ObjectType]
    RESOURCE: _ClassVar[ObjectType]

class ChangeKind(int, metaclass=_enum_type_wrapper.EnumTypeWrapper):
    __slots__ = ()
    CHANGE_KIND_UNSPECIFIED: _ClassVar[ChangeKind]
    CREATE: _ClassVar[ChangeKind]
    REMOVE: _ClassVar[ChangeKind]
    REPLACE: _ClassVar[ChangeKind]
    UPDATE: _ClassVar[ChangeKind]
    UPDATE_PARAMS: _ClassVar[ChangeKind]

class PlanPhase(int, metaclass=_enum_type_wrapper.EnumTypeWrapper):
    __slots__ = ()
    PLAN_PHASE_UNSPECIFIED: _ClassVar[PlanPhase]
    PREPARE: _ClassVar[PlanPhase]
    COMMIT: _ClassVar[PlanPhase]
    RETIRE: _ClassVar[PlanPhase]
OBJECT_TYPE_UNSPECIFIED: ObjectType
PORT: ObjectType
MODULE: ObjectType
CONNECTION: ObjectType
WORKER: ObjectType
TRAFFIC_CLASS: ObjectType
TASK: ObjectType
RESOURCE: ObjectType
CHANGE_KIND_UNSPECIFIED: ChangeKind
CREATE: ChangeKind
REMOVE: ChangeKind
REPLACE: ChangeKind
UPDATE: ChangeKind
UPDATE_PARAMS: ChangeKind
PLAN_PHASE_UNSPECIFIED: PlanPhase
PREPARE: PlanPhase
COMMIT: PlanPhase
RETIRE: PlanPhase

class Port(_message.Message):
    __slots__ = ("name", "driver", "num_rx_queues", "num_tx_queues", "rx_queue_size", "tx_queue_size", "arg")
    NAME_FIELD_NUMBER: _ClassVar[int]
    DRIVER_FIELD_NUMBER: _ClassVar[int]
    NUM_RX_QUEUES_FIELD_NUMBER: _ClassVar[int]
    NUM_TX_QUEUES_FIELD_NUMBER: _ClassVar[int]
    RX_QUEUE_SIZE_FIELD_NUMBER: _ClassVar[int]
    TX_QUEUE_SIZE_FIELD_NUMBER: _ClassVar[int]
    ARG_FIELD_NUMBER: _ClassVar[int]
    name: str
    driver: str
    num_rx_queues: int
    num_tx_queues: int
    rx_queue_size: int
    tx_queue_size: int
    arg: _any_pb2.Any
    def __init__(self, name: _Optional[str] = ..., driver: _Optional[str] = ..., num_rx_queues: _Optional[int] = ..., num_tx_queues: _Optional[int] = ..., rx_queue_size: _Optional[int] = ..., tx_queue_size: _Optional[int] = ..., arg: _Optional[_Union[_any_pb2.Any, _Mapping]] = ...) -> None: ...

class Module(_message.Message):
    __slots__ = ("name", "mclass", "arg")
    NAME_FIELD_NUMBER: _ClassVar[int]
    MCLASS_FIELD_NUMBER: _ClassVar[int]
    ARG_FIELD_NUMBER: _ClassVar[int]
    name: str
    mclass: str
    arg: _any_pb2.Any
    def __init__(self, name: _Optional[str] = ..., mclass: _Optional[str] = ..., arg: _Optional[_Union[_any_pb2.Any, _Mapping]] = ...) -> None: ...

class Connection(_message.Message):
    __slots__ = ("upstream", "ogate", "downstream", "igate", "skip_default_hooks")
    UPSTREAM_FIELD_NUMBER: _ClassVar[int]
    OGATE_FIELD_NUMBER: _ClassVar[int]
    DOWNSTREAM_FIELD_NUMBER: _ClassVar[int]
    IGATE_FIELD_NUMBER: _ClassVar[int]
    SKIP_DEFAULT_HOOKS_FIELD_NUMBER: _ClassVar[int]
    upstream: str
    ogate: int
    downstream: str
    igate: int
    skip_default_hooks: bool
    def __init__(self, upstream: _Optional[str] = ..., ogate: _Optional[int] = ..., downstream: _Optional[str] = ..., igate: _Optional[int] = ..., skip_default_hooks: _Optional[bool] = ...) -> None: ...

class Worker(_message.Message):
    __slots__ = ("wid", "core", "scheduler")
    WID_FIELD_NUMBER: _ClassVar[int]
    CORE_FIELD_NUMBER: _ClassVar[int]
    SCHEDULER_FIELD_NUMBER: _ClassVar[int]
    wid: int
    core: int
    scheduler: str
    def __init__(self, wid: _Optional[int] = ..., core: _Optional[int] = ..., scheduler: _Optional[str] = ...) -> None: ...

class TrafficClass(_message.Message):
    __slots__ = ("name", "parent", "policy", "resource", "wid", "priority", "share", "limit", "max_burst")
    class LimitEntry(_message.Message):
        __slots__ = ("key", "value")
        KEY_FIELD_NUMBER: _ClassVar[int]
        VALUE_FIELD_NUMBER: _ClassVar[int]
        key: str
        value: int
        def __init__(self, key: _Optional[str] = ..., value: _Optional[int] = ...) -> None: ...
    class MaxBurstEntry(_message.Message):
        __slots__ = ("key", "value")
        KEY_FIELD_NUMBER: _ClassVar[int]
        VALUE_FIELD_NUMBER: _ClassVar[int]
        key: str
        value: int
        def __init__(self, key: _Optional[str] = ..., value: _Optional[int] = ...) -> None: ...
    NAME_FIELD_NUMBER: _ClassVar[int]
    PARENT_FIELD_NUMBER: _ClassVar[int]
    POLICY_FIELD_NUMBER: _ClassVar[int]
    RESOURCE_FIELD_NUMBER: _ClassVar[int]
    WID_FIELD_NUMBER: _ClassVar[int]
    PRIORITY_FIELD_NUMBER: _ClassVar[int]
    SHARE_FIELD_NUMBER: _ClassVar[int]
    LIMIT_FIELD_NUMBER: _ClassVar[int]
    MAX_BURST_FIELD_NUMBER: _ClassVar[int]
    name: str
    parent: str
    policy: str
    resource: str
    wid: int
    priority: int
    share: int
    limit: _containers.ScalarMap[str, int]
    max_burst: _containers.ScalarMap[str, int]
    def __init__(self, name: _Optional[str] = ..., parent: _Optional[str] = ..., policy: _Optional[str] = ..., resource: _Optional[str] = ..., wid: _Optional[int] = ..., priority: _Optional[int] = ..., share: _Optional[int] = ..., limit: _Optional[_Mapping[str, int]] = ..., max_burst: _Optional[_Mapping[str, int]] = ...) -> None: ...

class TaskPlacement(_message.Message):
    __slots__ = ("module", "task", "parent", "wid", "priority", "share")
    MODULE_FIELD_NUMBER: _ClassVar[int]
    TASK_FIELD_NUMBER: _ClassVar[int]
    PARENT_FIELD_NUMBER: _ClassVar[int]
    WID_FIELD_NUMBER: _ClassVar[int]
    PRIORITY_FIELD_NUMBER: _ClassVar[int]
    SHARE_FIELD_NUMBER: _ClassVar[int]
    module: str
    task: int
    parent: str
    wid: int
    priority: int
    share: int
    def __init__(self, module: _Optional[str] = ..., task: _Optional[int] = ..., parent: _Optional[str] = ..., wid: _Optional[int] = ..., priority: _Optional[int] = ..., share: _Optional[int] = ...) -> None: ...

class ResourceEntry(_message.Message):
    __slots__ = ("key", "value")
    KEY_FIELD_NUMBER: _ClassVar[int]
    VALUE_FIELD_NUMBER: _ClassVar[int]
    key: _any_pb2.Any
    value: _any_pb2.Any
    def __init__(self, key: _Optional[_Union[_any_pb2.Any, _Mapping]] = ..., value: _Optional[_Union[_any_pb2.Any, _Mapping]] = ...) -> None: ...

class ResourceContents(_message.Message):
    __slots__ = ("name", "entries")
    NAME_FIELD_NUMBER: _ClassVar[int]
    ENTRIES_FIELD_NUMBER: _ClassVar[int]
    name: str
    entries: _containers.RepeatedCompositeFieldContainer[ResourceEntry]
    def __init__(self, name: _Optional[str] = ..., entries: _Optional[_Iterable[_Union[ResourceEntry, _Mapping]]] = ...) -> None: ...

class Pipeline(_message.Message):
    __slots__ = ("ports", "modules", "connections", "workers", "traffic_classes", "tasks", "resources")
    PORTS_FIELD_NUMBER: _ClassVar[int]
    MODULES_FIELD_NUMBER: _ClassVar[int]
    CONNECTIONS_FIELD_NUMBER: _ClassVar[int]
    WORKERS_FIELD_NUMBER: _ClassVar[int]
    TRAFFIC_CLASSES_FIELD_NUMBER: _ClassVar[int]
    TASKS_FIELD_NUMBER: _ClassVar[int]
    RESOURCES_FIELD_NUMBER: _ClassVar[int]
    ports: _containers.RepeatedCompositeFieldContainer[Port]
    modules: _containers.RepeatedCompositeFieldContainer[Module]
    connections: _containers.RepeatedCompositeFieldContainer[Connection]
    workers: _containers.RepeatedCompositeFieldContainer[Worker]
    traffic_classes: _containers.RepeatedCompositeFieldContainer[TrafficClass]
    tasks: _containers.RepeatedCompositeFieldContainer[TaskPlacement]
    resources: _containers.RepeatedCompositeFieldContainer[ResourceContents]
    def __init__(self, ports: _Optional[_Iterable[_Union[Port, _Mapping]]] = ..., modules: _Optional[_Iterable[_Union[Module, _Mapping]]] = ..., connections: _Optional[_Iterable[_Union[Connection, _Mapping]]] = ..., workers: _Optional[_Iterable[_Union[Worker, _Mapping]]] = ..., traffic_classes: _Optional[_Iterable[_Union[TrafficClass, _Mapping]]] = ..., tasks: _Optional[_Iterable[_Union[TaskPlacement, _Mapping]]] = ..., resources: _Optional[_Iterable[_Union[ResourceContents, _Mapping]]] = ...) -> None: ...

class Change(_message.Message):
    __slots__ = ("type", "object", "kind")
    TYPE_FIELD_NUMBER: _ClassVar[int]
    OBJECT_FIELD_NUMBER: _ClassVar[int]
    KIND_FIELD_NUMBER: _ClassVar[int]
    type: ObjectType
    object: str
    kind: ChangeKind
    def __init__(self, type: _Optional[_Union[ObjectType, str]] = ..., object: _Optional[str] = ..., kind: _Optional[_Union[ChangeKind, str]] = ...) -> None: ...

class ResourceDelta(_message.Message):
    __slots__ = ("name", "created", "updated", "removed", "checked")
    NAME_FIELD_NUMBER: _ClassVar[int]
    CREATED_FIELD_NUMBER: _ClassVar[int]
    UPDATED_FIELD_NUMBER: _ClassVar[int]
    REMOVED_FIELD_NUMBER: _ClassVar[int]
    CHECKED_FIELD_NUMBER: _ClassVar[int]
    name: str
    created: int
    updated: int
    removed: int
    checked: bool
    def __init__(self, name: _Optional[str] = ..., created: _Optional[int] = ..., updated: _Optional[int] = ..., removed: _Optional[int] = ..., checked: _Optional[bool] = ...) -> None: ...

class PipelineDiff(_message.Message):
    __slots__ = ("changes", "resources")
    CHANGES_FIELD_NUMBER: _ClassVar[int]
    RESOURCES_FIELD_NUMBER: _ClassVar[int]
    changes: _containers.RepeatedCompositeFieldContainer[Change]
    resources: _containers.RepeatedCompositeFieldContainer[ResourceDelta]
    def __init__(self, changes: _Optional[_Iterable[_Union[Change, _Mapping]]] = ..., resources: _Optional[_Iterable[_Union[ResourceDelta, _Mapping]]] = ...) -> None: ...

class PlanStep(_message.Message):
    __slots__ = ("phase", "operation", "type", "object")
    PHASE_FIELD_NUMBER: _ClassVar[int]
    OPERATION_FIELD_NUMBER: _ClassVar[int]
    TYPE_FIELD_NUMBER: _ClassVar[int]
    OBJECT_FIELD_NUMBER: _ClassVar[int]
    phase: PlanPhase
    operation: str
    type: ObjectType
    object: str
    def __init__(self, phase: _Optional[_Union[PlanPhase, str]] = ..., operation: _Optional[str] = ..., type: _Optional[_Union[ObjectType, str]] = ..., object: _Optional[str] = ...) -> None: ...

class WorkerPlacement(_message.Message):
    __slots__ = ("wid", "core", "numa_node", "cross_numa", "shares_core", "on_control_cpu")
    WID_FIELD_NUMBER: _ClassVar[int]
    CORE_FIELD_NUMBER: _ClassVar[int]
    NUMA_NODE_FIELD_NUMBER: _ClassVar[int]
    CROSS_NUMA_FIELD_NUMBER: _ClassVar[int]
    SHARES_CORE_FIELD_NUMBER: _ClassVar[int]
    ON_CONTROL_CPU_FIELD_NUMBER: _ClassVar[int]
    wid: int
    core: int
    numa_node: int
    cross_numa: bool
    shares_core: bool
    on_control_cpu: bool
    def __init__(self, wid: _Optional[int] = ..., core: _Optional[int] = ..., numa_node: _Optional[int] = ..., cross_numa: _Optional[bool] = ..., shares_core: _Optional[bool] = ..., on_control_cpu: _Optional[bool] = ...) -> None: ...

class GetPipelineRequest(_message.Message):
    __slots__ = ("include_resources", "resources")
    INCLUDE_RESOURCES_FIELD_NUMBER: _ClassVar[int]
    RESOURCES_FIELD_NUMBER: _ClassVar[int]
    include_resources: bool
    resources: _containers.RepeatedScalarFieldContainer[str]
    def __init__(self, include_resources: _Optional[bool] = ..., resources: _Optional[_Iterable[str]] = ...) -> None: ...

class GetPipelineResponse(_message.Message):
    __slots__ = ("pipeline", "generation", "daemon_epoch", "workers", "resource_generation")
    PIPELINE_FIELD_NUMBER: _ClassVar[int]
    GENERATION_FIELD_NUMBER: _ClassVar[int]
    DAEMON_EPOCH_FIELD_NUMBER: _ClassVar[int]
    WORKERS_FIELD_NUMBER: _ClassVar[int]
    RESOURCE_GENERATION_FIELD_NUMBER: _ClassVar[int]
    pipeline: Pipeline
    generation: int
    daemon_epoch: int
    workers: _containers.RepeatedCompositeFieldContainer[WorkerPlacement]
    resource_generation: int
    def __init__(self, pipeline: _Optional[_Union[Pipeline, _Mapping]] = ..., generation: _Optional[int] = ..., daemon_epoch: _Optional[int] = ..., workers: _Optional[_Iterable[_Union[WorkerPlacement, _Mapping]]] = ..., resource_generation: _Optional[int] = ...) -> None: ...

class ValidatePipelineRequest(_message.Message):
    __slots__ = ("pipeline",)
    PIPELINE_FIELD_NUMBER: _ClassVar[int]
    pipeline: Pipeline
    def __init__(self, pipeline: _Optional[_Union[Pipeline, _Mapping]] = ...) -> None: ...

class ValidatePipelineResponse(_message.Message):
    __slots__ = ("normalized",)
    NORMALIZED_FIELD_NUMBER: _ClassVar[int]
    normalized: Pipeline
    def __init__(self, normalized: _Optional[_Union[Pipeline, _Mapping]] = ...) -> None: ...

class DiffPipelineRequest(_message.Message):
    __slots__ = ("pipeline",)
    PIPELINE_FIELD_NUMBER: _ClassVar[int]
    pipeline: Pipeline
    def __init__(self, pipeline: _Optional[_Union[Pipeline, _Mapping]] = ...) -> None: ...

class DiffPipelineResponse(_message.Message):
    __slots__ = ("diff", "generation", "daemon_epoch", "resource_generation")
    DIFF_FIELD_NUMBER: _ClassVar[int]
    GENERATION_FIELD_NUMBER: _ClassVar[int]
    DAEMON_EPOCH_FIELD_NUMBER: _ClassVar[int]
    RESOURCE_GENERATION_FIELD_NUMBER: _ClassVar[int]
    diff: PipelineDiff
    generation: int
    daemon_epoch: int
    resource_generation: int
    def __init__(self, diff: _Optional[_Union[PipelineDiff, _Mapping]] = ..., generation: _Optional[int] = ..., daemon_epoch: _Optional[int] = ..., resource_generation: _Optional[int] = ...) -> None: ...

class PlanPipelineRequest(_message.Message):
    __slots__ = ("pipeline",)
    PIPELINE_FIELD_NUMBER: _ClassVar[int]
    pipeline: Pipeline
    def __init__(self, pipeline: _Optional[_Union[Pipeline, _Mapping]] = ...) -> None: ...

class PlanPipelineResponse(_message.Message):
    __slots__ = ("steps", "generation", "daemon_epoch", "resource_generation")
    STEPS_FIELD_NUMBER: _ClassVar[int]
    GENERATION_FIELD_NUMBER: _ClassVar[int]
    DAEMON_EPOCH_FIELD_NUMBER: _ClassVar[int]
    RESOURCE_GENERATION_FIELD_NUMBER: _ClassVar[int]
    steps: _containers.RepeatedCompositeFieldContainer[PlanStep]
    generation: int
    daemon_epoch: int
    resource_generation: int
    def __init__(self, steps: _Optional[_Iterable[_Union[PlanStep, _Mapping]]] = ..., generation: _Optional[int] = ..., daemon_epoch: _Optional[int] = ..., resource_generation: _Optional[int] = ...) -> None: ...

class ApplyPipelineRequest(_message.Message):
    __slots__ = ("pipeline", "expected_generation", "daemon_epoch", "expected_resource_generation")
    PIPELINE_FIELD_NUMBER: _ClassVar[int]
    EXPECTED_GENERATION_FIELD_NUMBER: _ClassVar[int]
    DAEMON_EPOCH_FIELD_NUMBER: _ClassVar[int]
    EXPECTED_RESOURCE_GENERATION_FIELD_NUMBER: _ClassVar[int]
    pipeline: Pipeline
    expected_generation: int
    daemon_epoch: int
    expected_resource_generation: int
    def __init__(self, pipeline: _Optional[_Union[Pipeline, _Mapping]] = ..., expected_generation: _Optional[int] = ..., daemon_epoch: _Optional[int] = ..., expected_resource_generation: _Optional[int] = ...) -> None: ...

class ApplyPipelineResponse(_message.Message):
    __slots__ = ("generation", "applied_ops", "workers_paused", "validation_us", "prepare_us", "paused_commit_us", "retire_us", "daemon_epoch", "resource_generation", "resource_ops")
    GENERATION_FIELD_NUMBER: _ClassVar[int]
    APPLIED_OPS_FIELD_NUMBER: _ClassVar[int]
    WORKERS_PAUSED_FIELD_NUMBER: _ClassVar[int]
    VALIDATION_US_FIELD_NUMBER: _ClassVar[int]
    PREPARE_US_FIELD_NUMBER: _ClassVar[int]
    PAUSED_COMMIT_US_FIELD_NUMBER: _ClassVar[int]
    RETIRE_US_FIELD_NUMBER: _ClassVar[int]
    DAEMON_EPOCH_FIELD_NUMBER: _ClassVar[int]
    RESOURCE_GENERATION_FIELD_NUMBER: _ClassVar[int]
    RESOURCE_OPS_FIELD_NUMBER: _ClassVar[int]
    generation: int
    applied_ops: int
    workers_paused: bool
    validation_us: int
    prepare_us: int
    paused_commit_us: int
    retire_us: int
    daemon_epoch: int
    resource_generation: int
    resource_ops: int
    def __init__(self, generation: _Optional[int] = ..., applied_ops: _Optional[int] = ..., workers_paused: _Optional[bool] = ..., validation_us: _Optional[int] = ..., prepare_us: _Optional[int] = ..., paused_commit_us: _Optional[int] = ..., retire_us: _Optional[int] = ..., daemon_epoch: _Optional[int] = ..., resource_generation: _Optional[int] = ..., resource_ops: _Optional[int] = ...) -> None: ...

class ErrorDetail(_message.Message):
    __slots__ = ("code", "errno_value", "message", "object", "field", "daemon_epoch")
    class Code(int, metaclass=_enum_type_wrapper.EnumTypeWrapper):
        __slots__ = ()
        CODE_UNSPECIFIED: _ClassVar[ErrorDetail.Code]
        INVALID_ARGUMENT: _ClassVar[ErrorDetail.Code]
        NOT_FOUND: _ClassVar[ErrorDetail.Code]
        ALREADY_EXISTS: _ClassVar[ErrorDetail.Code]
        CONFLICT: _ClassVar[ErrorDetail.Code]
        RESOURCE_BUSY: _ClassVar[ErrorDetail.Code]
        UNSUPPORTED_TRANSACTION: _ClassVar[ErrorDetail.Code]
        RESOURCE_FAILURE: _ClassVar[ErrorDetail.Code]
        INTERNAL: _ClassVar[ErrorDetail.Code]
        EPOCH_MISMATCH: _ClassVar[ErrorDetail.Code]
    CODE_UNSPECIFIED: ErrorDetail.Code
    INVALID_ARGUMENT: ErrorDetail.Code
    NOT_FOUND: ErrorDetail.Code
    ALREADY_EXISTS: ErrorDetail.Code
    CONFLICT: ErrorDetail.Code
    RESOURCE_BUSY: ErrorDetail.Code
    UNSUPPORTED_TRANSACTION: ErrorDetail.Code
    RESOURCE_FAILURE: ErrorDetail.Code
    INTERNAL: ErrorDetail.Code
    EPOCH_MISMATCH: ErrorDetail.Code
    CODE_FIELD_NUMBER: _ClassVar[int]
    ERRNO_VALUE_FIELD_NUMBER: _ClassVar[int]
    MESSAGE_FIELD_NUMBER: _ClassVar[int]
    OBJECT_FIELD_NUMBER: _ClassVar[int]
    FIELD_FIELD_NUMBER: _ClassVar[int]
    DAEMON_EPOCH_FIELD_NUMBER: _ClassVar[int]
    code: ErrorDetail.Code
    errno_value: int
    message: str
    object: str
    field: str
    daemon_epoch: int
    def __init__(self, code: _Optional[_Union[ErrorDetail.Code, str]] = ..., errno_value: _Optional[int] = ..., message: _Optional[str] = ..., object: _Optional[str] = ..., field: _Optional[str] = ..., daemon_epoch: _Optional[int] = ...) -> None: ...

class TransactionOp(_message.Message):
    __slots__ = ("resource", "erase", "key", "value")
    RESOURCE_FIELD_NUMBER: _ClassVar[int]
    ERASE_FIELD_NUMBER: _ClassVar[int]
    KEY_FIELD_NUMBER: _ClassVar[int]
    VALUE_FIELD_NUMBER: _ClassVar[int]
    resource: str
    erase: bool
    key: _any_pb2.Any
    value: _any_pb2.Any
    def __init__(self, resource: _Optional[str] = ..., erase: _Optional[bool] = ..., key: _Optional[_Union[_any_pb2.Any, _Mapping]] = ..., value: _Optional[_Union[_any_pb2.Any, _Mapping]] = ...) -> None: ...

class ApplyTransactionRequest(_message.Message):
    __slots__ = ("request_id", "expected_generation", "ops", "consistency", "daemon_epoch")
    class Consistency(int, metaclass=_enum_type_wrapper.EnumTypeWrapper):
        __slots__ = ()
        CONSISTENCY_UNSPECIFIED: _ClassVar[ApplyTransactionRequest.Consistency]
        CONSISTENCY_REFERENTIAL: _ClassVar[ApplyTransactionRequest.Consistency]
        CONSISTENCY_SCOPE_SNAPSHOT: _ClassVar[ApplyTransactionRequest.Consistency]
    CONSISTENCY_UNSPECIFIED: ApplyTransactionRequest.Consistency
    CONSISTENCY_REFERENTIAL: ApplyTransactionRequest.Consistency
    CONSISTENCY_SCOPE_SNAPSHOT: ApplyTransactionRequest.Consistency
    REQUEST_ID_FIELD_NUMBER: _ClassVar[int]
    EXPECTED_GENERATION_FIELD_NUMBER: _ClassVar[int]
    OPS_FIELD_NUMBER: _ClassVar[int]
    CONSISTENCY_FIELD_NUMBER: _ClassVar[int]
    DAEMON_EPOCH_FIELD_NUMBER: _ClassVar[int]
    request_id: str
    expected_generation: int
    ops: _containers.RepeatedCompositeFieldContainer[TransactionOp]
    consistency: ApplyTransactionRequest.Consistency
    daemon_epoch: int
    def __init__(self, request_id: _Optional[str] = ..., expected_generation: _Optional[int] = ..., ops: _Optional[_Iterable[_Union[TransactionOp, _Mapping]]] = ..., consistency: _Optional[_Union[ApplyTransactionRequest.Consistency, str]] = ..., daemon_epoch: _Optional[int] = ...) -> None: ...

class TransactionOpResult(_message.Message):
    __slots__ = ("status", "error")
    class Status(int, metaclass=_enum_type_wrapper.EnumTypeWrapper):
        __slots__ = ()
        STATUS_UNSPECIFIED: _ClassVar[TransactionOpResult.Status]
        STATUS_APPLIED: _ClassVar[TransactionOpResult.Status]
        STATUS_FAILED: _ClassVar[TransactionOpResult.Status]
        STATUS_NOT_APPLIED: _ClassVar[TransactionOpResult.Status]
    STATUS_UNSPECIFIED: TransactionOpResult.Status
    STATUS_APPLIED: TransactionOpResult.Status
    STATUS_FAILED: TransactionOpResult.Status
    STATUS_NOT_APPLIED: TransactionOpResult.Status
    STATUS_FIELD_NUMBER: _ClassVar[int]
    ERROR_FIELD_NUMBER: _ClassVar[int]
    status: TransactionOpResult.Status
    error: str
    def __init__(self, status: _Optional[_Union[TransactionOpResult.Status, str]] = ..., error: _Optional[str] = ...) -> None: ...

class TransactionRecord(_message.Message):
    __slots__ = ("request_id", "outcome", "generation", "ops", "visibility")
    class Outcome(int, metaclass=_enum_type_wrapper.EnumTypeWrapper):
        __slots__ = ()
        OUTCOME_UNSPECIFIED: _ClassVar[TransactionRecord.Outcome]
        OUTCOME_APPLIED: _ClassVar[TransactionRecord.Outcome]
        OUTCOME_REJECTED: _ClassVar[TransactionRecord.Outcome]
        OUTCOME_CONFLICT: _ClassVar[TransactionRecord.Outcome]
        OUTCOME_BUSY: _ClassVar[TransactionRecord.Outcome]
    OUTCOME_UNSPECIFIED: TransactionRecord.Outcome
    OUTCOME_APPLIED: TransactionRecord.Outcome
    OUTCOME_REJECTED: TransactionRecord.Outcome
    OUTCOME_CONFLICT: TransactionRecord.Outcome
    OUTCOME_BUSY: TransactionRecord.Outcome
    class Visibility(int, metaclass=_enum_type_wrapper.EnumTypeWrapper):
        __slots__ = ()
        VISIBILITY_UNSPECIFIED: _ClassVar[TransactionRecord.Visibility]
        VISIBILITY_DEPENDENCY_ORDERED: _ClassVar[TransactionRecord.Visibility]
        VISIBILITY_ATOMIC: _ClassVar[TransactionRecord.Visibility]
        VISIBILITY_SCOPE_SNAPSHOT: _ClassVar[TransactionRecord.Visibility]
    VISIBILITY_UNSPECIFIED: TransactionRecord.Visibility
    VISIBILITY_DEPENDENCY_ORDERED: TransactionRecord.Visibility
    VISIBILITY_ATOMIC: TransactionRecord.Visibility
    VISIBILITY_SCOPE_SNAPSHOT: TransactionRecord.Visibility
    REQUEST_ID_FIELD_NUMBER: _ClassVar[int]
    OUTCOME_FIELD_NUMBER: _ClassVar[int]
    GENERATION_FIELD_NUMBER: _ClassVar[int]
    OPS_FIELD_NUMBER: _ClassVar[int]
    VISIBILITY_FIELD_NUMBER: _ClassVar[int]
    request_id: str
    outcome: TransactionRecord.Outcome
    generation: int
    ops: _containers.RepeatedCompositeFieldContainer[TransactionOpResult]
    visibility: TransactionRecord.Visibility
    def __init__(self, request_id: _Optional[str] = ..., outcome: _Optional[_Union[TransactionRecord.Outcome, str]] = ..., generation: _Optional[int] = ..., ops: _Optional[_Iterable[_Union[TransactionOpResult, _Mapping]]] = ..., visibility: _Optional[_Union[TransactionRecord.Visibility, str]] = ...) -> None: ...

class ApplyTransactionResponse(_message.Message):
    __slots__ = ("record", "daemon_epoch", "replayed")
    RECORD_FIELD_NUMBER: _ClassVar[int]
    DAEMON_EPOCH_FIELD_NUMBER: _ClassVar[int]
    REPLAYED_FIELD_NUMBER: _ClassVar[int]
    record: TransactionRecord
    daemon_epoch: int
    replayed: bool
    def __init__(self, record: _Optional[_Union[TransactionRecord, _Mapping]] = ..., daemon_epoch: _Optional[int] = ..., replayed: _Optional[bool] = ...) -> None: ...

class GetTransactionRequest(_message.Message):
    __slots__ = ("request_id",)
    REQUEST_ID_FIELD_NUMBER: _ClassVar[int]
    request_id: str
    def __init__(self, request_id: _Optional[str] = ...) -> None: ...

class GetTransactionResponse(_message.Message):
    __slots__ = ("known", "record", "daemon_epoch")
    KNOWN_FIELD_NUMBER: _ClassVar[int]
    RECORD_FIELD_NUMBER: _ClassVar[int]
    DAEMON_EPOCH_FIELD_NUMBER: _ClassVar[int]
    known: bool
    record: TransactionRecord
    daemon_epoch: int
    def __init__(self, known: _Optional[bool] = ..., record: _Optional[_Union[TransactionRecord, _Mapping]] = ..., daemon_epoch: _Optional[int] = ...) -> None: ...

class ListTransactionResourcesRequest(_message.Message):
    __slots__ = ()
    def __init__(self) -> None: ...

class TransactionResource(_message.Message):
    __slots__ = ("name", "key_type", "value_type", "kind", "requires_paused_workers")
    class Kind(int, metaclass=_enum_type_wrapper.EnumTypeWrapper):
        __slots__ = ()
        KIND_UNSPECIFIED: _ClassVar[TransactionResource.Kind]
        KIND_MAP: _ClassVar[TransactionResource.Kind]
        KIND_SINGLETON: _ClassVar[TransactionResource.Kind]
    KIND_UNSPECIFIED: TransactionResource.Kind
    KIND_MAP: TransactionResource.Kind
    KIND_SINGLETON: TransactionResource.Kind
    NAME_FIELD_NUMBER: _ClassVar[int]
    KEY_TYPE_FIELD_NUMBER: _ClassVar[int]
    VALUE_TYPE_FIELD_NUMBER: _ClassVar[int]
    KIND_FIELD_NUMBER: _ClassVar[int]
    REQUIRES_PAUSED_WORKERS_FIELD_NUMBER: _ClassVar[int]
    name: str
    key_type: str
    value_type: str
    kind: TransactionResource.Kind
    requires_paused_workers: bool
    def __init__(self, name: _Optional[str] = ..., key_type: _Optional[str] = ..., value_type: _Optional[str] = ..., kind: _Optional[_Union[TransactionResource.Kind, str]] = ..., requires_paused_workers: _Optional[bool] = ...) -> None: ...

class ListTransactionResourcesResponse(_message.Message):
    __slots__ = ("resources", "generation", "daemon_epoch")
    RESOURCES_FIELD_NUMBER: _ClassVar[int]
    GENERATION_FIELD_NUMBER: _ClassVar[int]
    DAEMON_EPOCH_FIELD_NUMBER: _ClassVar[int]
    resources: _containers.RepeatedCompositeFieldContainer[TransactionResource]
    generation: int
    daemon_epoch: int
    def __init__(self, resources: _Optional[_Iterable[_Union[TransactionResource, _Mapping]]] = ..., generation: _Optional[int] = ..., daemon_epoch: _Optional[int] = ...) -> None: ...

class GetCapabilitiesRequest(_message.Message):
    __slots__ = ()
    def __init__(self) -> None: ...

class PluginInfo(_message.Message):
    __slots__ = ("name", "version", "path", "required_capabilities")
    NAME_FIELD_NUMBER: _ClassVar[int]
    VERSION_FIELD_NUMBER: _ClassVar[int]
    PATH_FIELD_NUMBER: _ClassVar[int]
    REQUIRED_CAPABILITIES_FIELD_NUMBER: _ClassVar[int]
    name: str
    version: str
    path: str
    required_capabilities: int
    def __init__(self, name: _Optional[str] = ..., version: _Optional[str] = ..., path: _Optional[str] = ..., required_capabilities: _Optional[int] = ...) -> None: ...

class PortInfo(_message.Message):
    __slots__ = ("name", "driver", "rx_queues", "tx_queues", "tx_ipv4_checksum", "tx_udp_checksum", "tx_tcp_checksum", "tx_outer_ipv4_checksum", "tx_outer_udp_checksum", "tx_multi_segment", "symmetric_rss", "rss_signature")
    NAME_FIELD_NUMBER: _ClassVar[int]
    DRIVER_FIELD_NUMBER: _ClassVar[int]
    RX_QUEUES_FIELD_NUMBER: _ClassVar[int]
    TX_QUEUES_FIELD_NUMBER: _ClassVar[int]
    TX_IPV4_CHECKSUM_FIELD_NUMBER: _ClassVar[int]
    TX_UDP_CHECKSUM_FIELD_NUMBER: _ClassVar[int]
    TX_TCP_CHECKSUM_FIELD_NUMBER: _ClassVar[int]
    TX_OUTER_IPV4_CHECKSUM_FIELD_NUMBER: _ClassVar[int]
    TX_OUTER_UDP_CHECKSUM_FIELD_NUMBER: _ClassVar[int]
    TX_MULTI_SEGMENT_FIELD_NUMBER: _ClassVar[int]
    SYMMETRIC_RSS_FIELD_NUMBER: _ClassVar[int]
    RSS_SIGNATURE_FIELD_NUMBER: _ClassVar[int]
    name: str
    driver: str
    rx_queues: int
    tx_queues: int
    tx_ipv4_checksum: bool
    tx_udp_checksum: bool
    tx_tcp_checksum: bool
    tx_outer_ipv4_checksum: bool
    tx_outer_udp_checksum: bool
    tx_multi_segment: bool
    symmetric_rss: bool
    rss_signature: int
    def __init__(self, name: _Optional[str] = ..., driver: _Optional[str] = ..., rx_queues: _Optional[int] = ..., tx_queues: _Optional[int] = ..., tx_ipv4_checksum: _Optional[bool] = ..., tx_udp_checksum: _Optional[bool] = ..., tx_tcp_checksum: _Optional[bool] = ..., tx_outer_ipv4_checksum: _Optional[bool] = ..., tx_outer_udp_checksum: _Optional[bool] = ..., tx_multi_segment: _Optional[bool] = ..., symmetric_rss: _Optional[bool] = ..., rss_signature: _Optional[int] = ...) -> None: ...

class GetCapabilitiesResponse(_message.Message):
    __slots__ = ("daemon_version", "rpcs", "plugin_api_version", "plugin_capabilities", "module_classes", "plugins", "ports", "resources", "daemon_epoch", "placement", "memory_node", "simd", "simd_width", "dpdk_simd_width")
    DAEMON_VERSION_FIELD_NUMBER: _ClassVar[int]
    RPCS_FIELD_NUMBER: _ClassVar[int]
    PLUGIN_API_VERSION_FIELD_NUMBER: _ClassVar[int]
    PLUGIN_CAPABILITIES_FIELD_NUMBER: _ClassVar[int]
    MODULE_CLASSES_FIELD_NUMBER: _ClassVar[int]
    PLUGINS_FIELD_NUMBER: _ClassVar[int]
    PORTS_FIELD_NUMBER: _ClassVar[int]
    RESOURCES_FIELD_NUMBER: _ClassVar[int]
    DAEMON_EPOCH_FIELD_NUMBER: _ClassVar[int]
    PLACEMENT_FIELD_NUMBER: _ClassVar[int]
    MEMORY_NODE_FIELD_NUMBER: _ClassVar[int]
    SIMD_FIELD_NUMBER: _ClassVar[int]
    SIMD_WIDTH_FIELD_NUMBER: _ClassVar[int]
    DPDK_SIMD_WIDTH_FIELD_NUMBER: _ClassVar[int]
    daemon_version: str
    rpcs: _containers.RepeatedScalarFieldContainer[str]
    plugin_api_version: int
    plugin_capabilities: int
    module_classes: _containers.RepeatedScalarFieldContainer[str]
    plugins: _containers.RepeatedCompositeFieldContainer[PluginInfo]
    ports: _containers.RepeatedCompositeFieldContainer[PortInfo]
    resources: _containers.RepeatedCompositeFieldContainer[TransactionResource]
    daemon_epoch: int
    placement: str
    memory_node: int
    simd: str
    simd_width: int
    dpdk_simd_width: int
    def __init__(self, daemon_version: _Optional[str] = ..., rpcs: _Optional[_Iterable[str]] = ..., plugin_api_version: _Optional[int] = ..., plugin_capabilities: _Optional[int] = ..., module_classes: _Optional[_Iterable[str]] = ..., plugins: _Optional[_Iterable[_Union[PluginInfo, _Mapping]]] = ..., ports: _Optional[_Iterable[_Union[PortInfo, _Mapping]]] = ..., resources: _Optional[_Iterable[_Union[TransactionResource, _Mapping]]] = ..., daemon_epoch: _Optional[int] = ..., placement: _Optional[str] = ..., memory_node: _Optional[int] = ..., simd: _Optional[str] = ..., simd_width: _Optional[int] = ..., dpdk_simd_width: _Optional[int] = ...) -> None: ...

class GetSchemaRequest(_message.Message):
    __slots__ = ()
    def __init__(self) -> None: ...

class ResourceSchema(_message.Message):
    __slots__ = ("table", "kind", "key_type", "value_type", "requires_paused_workers")
    TABLE_FIELD_NUMBER: _ClassVar[int]
    KIND_FIELD_NUMBER: _ClassVar[int]
    KEY_TYPE_FIELD_NUMBER: _ClassVar[int]
    VALUE_TYPE_FIELD_NUMBER: _ClassVar[int]
    REQUIRES_PAUSED_WORKERS_FIELD_NUMBER: _ClassVar[int]
    table: str
    kind: TransactionResource.Kind
    key_type: str
    value_type: str
    requires_paused_workers: bool
    def __init__(self, table: _Optional[str] = ..., kind: _Optional[_Union[TransactionResource.Kind, str]] = ..., key_type: _Optional[str] = ..., value_type: _Optional[str] = ..., requires_paused_workers: _Optional[bool] = ...) -> None: ...

class ActionSchema(_message.Message):
    __slots__ = ("name", "arg_type", "result_type", "thread_safe")
    NAME_FIELD_NUMBER: _ClassVar[int]
    ARG_TYPE_FIELD_NUMBER: _ClassVar[int]
    RESULT_TYPE_FIELD_NUMBER: _ClassVar[int]
    THREAD_SAFE_FIELD_NUMBER: _ClassVar[int]
    name: str
    arg_type: str
    result_type: str
    thread_safe: bool
    def __init__(self, name: _Optional[str] = ..., arg_type: _Optional[str] = ..., result_type: _Optional[str] = ..., thread_safe: _Optional[bool] = ...) -> None: ...

class ModuleClassSchema(_message.Message):
    __slots__ = ("name", "help", "init_arg_type", "igates", "ogates", "actions", "resources")
    NAME_FIELD_NUMBER: _ClassVar[int]
    HELP_FIELD_NUMBER: _ClassVar[int]
    INIT_ARG_TYPE_FIELD_NUMBER: _ClassVar[int]
    IGATES_FIELD_NUMBER: _ClassVar[int]
    OGATES_FIELD_NUMBER: _ClassVar[int]
    ACTIONS_FIELD_NUMBER: _ClassVar[int]
    RESOURCES_FIELD_NUMBER: _ClassVar[int]
    name: str
    help: str
    init_arg_type: str
    igates: int
    ogates: int
    actions: _containers.RepeatedCompositeFieldContainer[ActionSchema]
    resources: _containers.RepeatedCompositeFieldContainer[ResourceSchema]
    def __init__(self, name: _Optional[str] = ..., help: _Optional[str] = ..., init_arg_type: _Optional[str] = ..., igates: _Optional[int] = ..., ogates: _Optional[int] = ..., actions: _Optional[_Iterable[_Union[ActionSchema, _Mapping]]] = ..., resources: _Optional[_Iterable[_Union[ResourceSchema, _Mapping]]] = ...) -> None: ...

class PortDriverSchema(_message.Message):
    __slots__ = ("name", "help", "init_arg_type")
    NAME_FIELD_NUMBER: _ClassVar[int]
    HELP_FIELD_NUMBER: _ClassVar[int]
    INIT_ARG_TYPE_FIELD_NUMBER: _ClassVar[int]
    name: str
    help: str
    init_arg_type: str
    def __init__(self, name: _Optional[str] = ..., help: _Optional[str] = ..., init_arg_type: _Optional[str] = ...) -> None: ...

class GetSchemaResponse(_message.Message):
    __slots__ = ("module_classes", "port_drivers", "descriptors", "daemon_epoch")
    MODULE_CLASSES_FIELD_NUMBER: _ClassVar[int]
    PORT_DRIVERS_FIELD_NUMBER: _ClassVar[int]
    DESCRIPTORS_FIELD_NUMBER: _ClassVar[int]
    DAEMON_EPOCH_FIELD_NUMBER: _ClassVar[int]
    module_classes: _containers.RepeatedCompositeFieldContainer[ModuleClassSchema]
    port_drivers: _containers.RepeatedCompositeFieldContainer[PortDriverSchema]
    descriptors: _descriptor_pb2.FileDescriptorSet
    daemon_epoch: int
    def __init__(self, module_classes: _Optional[_Iterable[_Union[ModuleClassSchema, _Mapping]]] = ..., port_drivers: _Optional[_Iterable[_Union[PortDriverSchema, _Mapping]]] = ..., descriptors: _Optional[_Union[_descriptor_pb2.FileDescriptorSet, _Mapping]] = ..., daemon_epoch: _Optional[int] = ...) -> None: ...

class WatchEventsRequest(_message.Message):
    __slots__ = ("from_sequence", "types", "daemon_epoch")
    FROM_SEQUENCE_FIELD_NUMBER: _ClassVar[int]
    TYPES_FIELD_NUMBER: _ClassVar[int]
    DAEMON_EPOCH_FIELD_NUMBER: _ClassVar[int]
    from_sequence: int
    types: _containers.RepeatedScalarFieldContainer[str]
    daemon_epoch: int
    def __init__(self, from_sequence: _Optional[int] = ..., types: _Optional[_Iterable[str]] = ..., daemon_epoch: _Optional[int] = ...) -> None: ...

class Event(_message.Message):
    __slots__ = ("sequence", "daemon_epoch", "time_ns", "type", "source", "generation", "fields", "gap_from", "gap_to")
    class FieldsEntry(_message.Message):
        __slots__ = ("key", "value")
        KEY_FIELD_NUMBER: _ClassVar[int]
        VALUE_FIELD_NUMBER: _ClassVar[int]
        key: str
        value: str
        def __init__(self, key: _Optional[str] = ..., value: _Optional[str] = ...) -> None: ...
    SEQUENCE_FIELD_NUMBER: _ClassVar[int]
    DAEMON_EPOCH_FIELD_NUMBER: _ClassVar[int]
    TIME_NS_FIELD_NUMBER: _ClassVar[int]
    TYPE_FIELD_NUMBER: _ClassVar[int]
    SOURCE_FIELD_NUMBER: _ClassVar[int]
    GENERATION_FIELD_NUMBER: _ClassVar[int]
    FIELDS_FIELD_NUMBER: _ClassVar[int]
    GAP_FROM_FIELD_NUMBER: _ClassVar[int]
    GAP_TO_FIELD_NUMBER: _ClassVar[int]
    sequence: int
    daemon_epoch: int
    time_ns: int
    type: str
    source: str
    generation: int
    fields: _containers.ScalarMap[str, str]
    gap_from: int
    gap_to: int
    def __init__(self, sequence: _Optional[int] = ..., daemon_epoch: _Optional[int] = ..., time_ns: _Optional[int] = ..., type: _Optional[str] = ..., source: _Optional[str] = ..., generation: _Optional[int] = ..., fields: _Optional[_Mapping[str, str]] = ..., gap_from: _Optional[int] = ..., gap_to: _Optional[int] = ...) -> None: ...

class ListMetricsRequest(_message.Message):
    __slots__ = ()
    def __init__(self) -> None: ...

class MetricSample(_message.Message):
    __slots__ = ("name", "kind", "help", "labels", "value")
    class Kind(int, metaclass=_enum_type_wrapper.EnumTypeWrapper):
        __slots__ = ()
        KIND_UNSPECIFIED: _ClassVar[MetricSample.Kind]
        KIND_COUNTER: _ClassVar[MetricSample.Kind]
        KIND_GAUGE: _ClassVar[MetricSample.Kind]
    KIND_UNSPECIFIED: MetricSample.Kind
    KIND_COUNTER: MetricSample.Kind
    KIND_GAUGE: MetricSample.Kind
    class LabelsEntry(_message.Message):
        __slots__ = ("key", "value")
        KEY_FIELD_NUMBER: _ClassVar[int]
        VALUE_FIELD_NUMBER: _ClassVar[int]
        key: str
        value: str
        def __init__(self, key: _Optional[str] = ..., value: _Optional[str] = ...) -> None: ...
    NAME_FIELD_NUMBER: _ClassVar[int]
    KIND_FIELD_NUMBER: _ClassVar[int]
    HELP_FIELD_NUMBER: _ClassVar[int]
    LABELS_FIELD_NUMBER: _ClassVar[int]
    VALUE_FIELD_NUMBER: _ClassVar[int]
    name: str
    kind: MetricSample.Kind
    help: str
    labels: _containers.ScalarMap[str, str]
    value: float
    def __init__(self, name: _Optional[str] = ..., kind: _Optional[_Union[MetricSample.Kind, str]] = ..., help: _Optional[str] = ..., labels: _Optional[_Mapping[str, str]] = ..., value: _Optional[float] = ...) -> None: ...

class ListMetricsResponse(_message.Message):
    __slots__ = ("samples", "daemon_epoch")
    SAMPLES_FIELD_NUMBER: _ClassVar[int]
    DAEMON_EPOCH_FIELD_NUMBER: _ClassVar[int]
    samples: _containers.RepeatedCompositeFieldContainer[MetricSample]
    daemon_epoch: int
    def __init__(self, samples: _Optional[_Iterable[_Union[MetricSample, _Mapping]]] = ..., daemon_epoch: _Optional[int] = ...) -> None: ...

class InvokeActionRequest(_message.Message):
    __slots__ = ("daemon_epoch", "module", "action", "arg")
    DAEMON_EPOCH_FIELD_NUMBER: _ClassVar[int]
    MODULE_FIELD_NUMBER: _ClassVar[int]
    ACTION_FIELD_NUMBER: _ClassVar[int]
    ARG_FIELD_NUMBER: _ClassVar[int]
    daemon_epoch: int
    module: str
    action: str
    arg: _any_pb2.Any
    def __init__(self, daemon_epoch: _Optional[int] = ..., module: _Optional[str] = ..., action: _Optional[str] = ..., arg: _Optional[_Union[_any_pb2.Any, _Mapping]] = ...) -> None: ...

class InvokeActionResponse(_message.Message):
    __slots__ = ("result", "daemon_epoch")
    RESULT_FIELD_NUMBER: _ClassVar[int]
    DAEMON_EPOCH_FIELD_NUMBER: _ClassVar[int]
    result: _any_pb2.Any
    daemon_epoch: int
    def __init__(self, result: _Optional[_Union[_any_pb2.Any, _Mapping]] = ..., daemon_epoch: _Optional[int] = ...) -> None: ...

class ImportPluginRequest(_message.Message):
    __slots__ = ("path", "daemon_epoch")
    PATH_FIELD_NUMBER: _ClassVar[int]
    DAEMON_EPOCH_FIELD_NUMBER: _ClassVar[int]
    path: str
    daemon_epoch: int
    def __init__(self, path: _Optional[str] = ..., daemon_epoch: _Optional[int] = ...) -> None: ...

class ImportPluginResponse(_message.Message):
    __slots__ = ("daemon_epoch",)
    DAEMON_EPOCH_FIELD_NUMBER: _ClassVar[int]
    daemon_epoch: int
    def __init__(self, daemon_epoch: _Optional[int] = ...) -> None: ...

class UnloadPluginRequest(_message.Message):
    __slots__ = ("path", "daemon_epoch")
    PATH_FIELD_NUMBER: _ClassVar[int]
    DAEMON_EPOCH_FIELD_NUMBER: _ClassVar[int]
    path: str
    daemon_epoch: int
    def __init__(self, path: _Optional[str] = ..., daemon_epoch: _Optional[int] = ...) -> None: ...

class UnloadPluginResponse(_message.Message):
    __slots__ = ("daemon_epoch",)
    DAEMON_EPOCH_FIELD_NUMBER: _ClassVar[int]
    daemon_epoch: int
    def __init__(self, daemon_epoch: _Optional[int] = ...) -> None: ...

class GetPortStatusRequest(_message.Message):
    __slots__ = ("names",)
    NAMES_FIELD_NUMBER: _ClassVar[int]
    names: _containers.RepeatedScalarFieldContainer[str]
    def __init__(self, names: _Optional[_Iterable[str]] = ...) -> None: ...

class PortStatus(_message.Message):
    __slots__ = ("name", "driver", "link", "rx", "tx", "numa_node", "cross_numa")
    class Link(_message.Message):
        __slots__ = ("up", "speed_mbps", "full_duplex", "autoneg")
        UP_FIELD_NUMBER: _ClassVar[int]
        SPEED_MBPS_FIELD_NUMBER: _ClassVar[int]
        FULL_DUPLEX_FIELD_NUMBER: _ClassVar[int]
        AUTONEG_FIELD_NUMBER: _ClassVar[int]
        up: bool
        speed_mbps: int
        full_duplex: bool
        autoneg: bool
        def __init__(self, up: _Optional[bool] = ..., speed_mbps: _Optional[int] = ..., full_duplex: _Optional[bool] = ..., autoneg: _Optional[bool] = ...) -> None: ...
    class Counters(_message.Message):
        __slots__ = ("packets", "bytes", "dropped", "prepare_errors")
        PACKETS_FIELD_NUMBER: _ClassVar[int]
        BYTES_FIELD_NUMBER: _ClassVar[int]
        DROPPED_FIELD_NUMBER: _ClassVar[int]
        PREPARE_ERRORS_FIELD_NUMBER: _ClassVar[int]
        packets: int
        bytes: int
        dropped: int
        prepare_errors: int
        def __init__(self, packets: _Optional[int] = ..., bytes: _Optional[int] = ..., dropped: _Optional[int] = ..., prepare_errors: _Optional[int] = ...) -> None: ...
    NAME_FIELD_NUMBER: _ClassVar[int]
    DRIVER_FIELD_NUMBER: _ClassVar[int]
    LINK_FIELD_NUMBER: _ClassVar[int]
    RX_FIELD_NUMBER: _ClassVar[int]
    TX_FIELD_NUMBER: _ClassVar[int]
    NUMA_NODE_FIELD_NUMBER: _ClassVar[int]
    CROSS_NUMA_FIELD_NUMBER: _ClassVar[int]
    name: str
    driver: str
    link: PortStatus.Link
    rx: PortStatus.Counters
    tx: PortStatus.Counters
    numa_node: int
    cross_numa: bool
    def __init__(self, name: _Optional[str] = ..., driver: _Optional[str] = ..., link: _Optional[_Union[PortStatus.Link, _Mapping]] = ..., rx: _Optional[_Union[PortStatus.Counters, _Mapping]] = ..., tx: _Optional[_Union[PortStatus.Counters, _Mapping]] = ..., numa_node: _Optional[int] = ..., cross_numa: _Optional[bool] = ...) -> None: ...

class GetPortStatusResponse(_message.Message):
    __slots__ = ("ports", "daemon_epoch")
    PORTS_FIELD_NUMBER: _ClassVar[int]
    DAEMON_EPOCH_FIELD_NUMBER: _ClassVar[int]
    ports: _containers.RepeatedCompositeFieldContainer[PortStatus]
    daemon_epoch: int
    def __init__(self, ports: _Optional[_Iterable[_Union[PortStatus, _Mapping]]] = ..., daemon_epoch: _Optional[int] = ...) -> None: ...

class GetPacketPoolsRequest(_message.Message):
    __slots__ = ()
    def __init__(self) -> None: ...

class PacketPool(_message.Message):
    __slots__ = ("socket", "size", "cache_size", "element_size", "populated", "available", "in_use")
    SOCKET_FIELD_NUMBER: _ClassVar[int]
    SIZE_FIELD_NUMBER: _ClassVar[int]
    CACHE_SIZE_FIELD_NUMBER: _ClassVar[int]
    ELEMENT_SIZE_FIELD_NUMBER: _ClassVar[int]
    POPULATED_FIELD_NUMBER: _ClassVar[int]
    AVAILABLE_FIELD_NUMBER: _ClassVar[int]
    IN_USE_FIELD_NUMBER: _ClassVar[int]
    socket: int
    size: int
    cache_size: int
    element_size: int
    populated: int
    available: int
    in_use: int
    def __init__(self, socket: _Optional[int] = ..., size: _Optional[int] = ..., cache_size: _Optional[int] = ..., element_size: _Optional[int] = ..., populated: _Optional[int] = ..., available: _Optional[int] = ..., in_use: _Optional[int] = ...) -> None: ...

class GetPacketPoolsResponse(_message.Message):
    __slots__ = ("pools", "daemon_epoch")
    POOLS_FIELD_NUMBER: _ClassVar[int]
    DAEMON_EPOCH_FIELD_NUMBER: _ClassVar[int]
    pools: _containers.RepeatedCompositeFieldContainer[PacketPool]
    daemon_epoch: int
    def __init__(self, pools: _Optional[_Iterable[_Union[PacketPool, _Mapping]]] = ..., daemon_epoch: _Optional[int] = ...) -> None: ...

class SetCaptureRequest(_message.Message):
    __slots__ = ("module", "direction", "gate", "format", "fifo", "enable", "daemon_epoch")
    class Direction(int, metaclass=_enum_type_wrapper.EnumTypeWrapper):
        __slots__ = ()
        DIRECTION_UNSPECIFIED: _ClassVar[SetCaptureRequest.Direction]
        DIRECTION_INPUT: _ClassVar[SetCaptureRequest.Direction]
        DIRECTION_OUTPUT: _ClassVar[SetCaptureRequest.Direction]
    DIRECTION_UNSPECIFIED: SetCaptureRequest.Direction
    DIRECTION_INPUT: SetCaptureRequest.Direction
    DIRECTION_OUTPUT: SetCaptureRequest.Direction
    class Format(int, metaclass=_enum_type_wrapper.EnumTypeWrapper):
        __slots__ = ()
        FORMAT_UNSPECIFIED: _ClassVar[SetCaptureRequest.Format]
        FORMAT_PCAP: _ClassVar[SetCaptureRequest.Format]
        FORMAT_PCAPNG: _ClassVar[SetCaptureRequest.Format]
    FORMAT_UNSPECIFIED: SetCaptureRequest.Format
    FORMAT_PCAP: SetCaptureRequest.Format
    FORMAT_PCAPNG: SetCaptureRequest.Format
    MODULE_FIELD_NUMBER: _ClassVar[int]
    DIRECTION_FIELD_NUMBER: _ClassVar[int]
    GATE_FIELD_NUMBER: _ClassVar[int]
    FORMAT_FIELD_NUMBER: _ClassVar[int]
    FIFO_FIELD_NUMBER: _ClassVar[int]
    ENABLE_FIELD_NUMBER: _ClassVar[int]
    DAEMON_EPOCH_FIELD_NUMBER: _ClassVar[int]
    module: str
    direction: SetCaptureRequest.Direction
    gate: int
    format: SetCaptureRequest.Format
    fifo: str
    enable: bool
    daemon_epoch: int
    def __init__(self, module: _Optional[str] = ..., direction: _Optional[_Union[SetCaptureRequest.Direction, str]] = ..., gate: _Optional[int] = ..., format: _Optional[_Union[SetCaptureRequest.Format, str]] = ..., fifo: _Optional[str] = ..., enable: _Optional[bool] = ..., daemon_epoch: _Optional[int] = ...) -> None: ...

class SetCaptureResponse(_message.Message):
    __slots__ = ("daemon_epoch",)
    DAEMON_EPOCH_FIELD_NUMBER: _ClassVar[int]
    daemon_epoch: int
    def __init__(self, daemon_epoch: _Optional[int] = ...) -> None: ...

class PauseWorkersRequest(_message.Message):
    __slots__ = ("daemon_epoch",)
    DAEMON_EPOCH_FIELD_NUMBER: _ClassVar[int]
    daemon_epoch: int
    def __init__(self, daemon_epoch: _Optional[int] = ...) -> None: ...

class PauseWorkersResponse(_message.Message):
    __slots__ = ("daemon_epoch",)
    DAEMON_EPOCH_FIELD_NUMBER: _ClassVar[int]
    daemon_epoch: int
    def __init__(self, daemon_epoch: _Optional[int] = ...) -> None: ...

class ResumeWorkersRequest(_message.Message):
    __slots__ = ("daemon_epoch",)
    DAEMON_EPOCH_FIELD_NUMBER: _ClassVar[int]
    daemon_epoch: int
    def __init__(self, daemon_epoch: _Optional[int] = ...) -> None: ...

class ResumeWorkersResponse(_message.Message):
    __slots__ = ("daemon_epoch",)
    DAEMON_EPOCH_FIELD_NUMBER: _ClassVar[int]
    daemon_epoch: int
    def __init__(self, daemon_epoch: _Optional[int] = ...) -> None: ...

class ShutdownRequest(_message.Message):
    __slots__ = ("daemon_epoch",)
    DAEMON_EPOCH_FIELD_NUMBER: _ClassVar[int]
    daemon_epoch: int
    def __init__(self, daemon_epoch: _Optional[int] = ...) -> None: ...

class ShutdownResponse(_message.Message):
    __slots__ = ("daemon_epoch",)
    DAEMON_EPOCH_FIELD_NUMBER: _ClassVar[int]
    daemon_epoch: int
    def __init__(self, daemon_epoch: _Optional[int] = ...) -> None: ...
