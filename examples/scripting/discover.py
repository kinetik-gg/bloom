"""Inspect the versioned host vocabulary without changing a document."""
import inspect
import bloom

print("Bloom", bloom.app.version, "facade", bloom.app.facade_version)
for operation_id, operation in bloom.ops.registry().items():
    print(operation_id, inspect.signature(operation))
print("Project:", bloom.context.project.name)
print("Revision:", bloom.data.snapshot()["revision"])
