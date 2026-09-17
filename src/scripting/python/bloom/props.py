"""Reserved namespace; contribution APIs are deferred by ADR 0022 and ADR 0012."""

def __getattr__(name):
    raise NotImplementedError(f"bloom.props.{name} is deferred in v1; see ADR 0022 and ADR 0012")
