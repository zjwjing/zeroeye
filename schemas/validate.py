#!/usr/bin/env python3
"""Validate example configurations against the app-config JSON Schema."""

import json
import sys
from pathlib import Path

try:
    from jsonschema import Draft7Validator, ValidationError
except ImportError:
    print("Installing jsonschema...")
    import subprocess
    subprocess.check_call([sys.executable, "-m", "pip", "install", "jsonschema", "-q"])
    from jsonschema import Draft7Validator, ValidationError


SCHEMA_PATH = Path(__file__).parent / "app-config.schema.json"
EXAMPLES_DIR = Path(__file__).parent / "examples"


def validate_example(schema: dict, example_path: Path, should_pass: bool) -> bool:
    """Validate a single example against the schema."""
    with open(example_path) as f:
        config = json.load(f)

    validator = Draft7Validator(schema)
    errors = list(validator.iter_errors(config))

    if should_pass:
        if errors:
            print(f"FAIL: {example_path.name} should be valid but has {len(errors)} error(s):")
            for err in errors[:5]:
                print(f"  - {err.json_path}: {err.message}")
            return False
        else:
            print(f"PASS: {example_path.name} is valid")
            return True
    else:
        if errors:
            print(f"PASS: {example_path.name} is correctly invalid ({len(errors)} error(s))")
            return True
        else:
            print(f"FAIL: {example_path.name} should be invalid but passed validation")
            return False


def main() -> int:
    with open(SCHEMA_PATH) as f:
        schema = json.load(f)

    print(f"Schema: {SCHEMA_PATH.name}")
    print(f"Schema draft: {schema.get('$schema', 'unknown')}")
    print()

    results = []

    # Valid examples
    for example in sorted(EXAMPLES_DIR.glob("config-valid-*.json")):
        results.append(validate_example(schema, example, should_pass=True))

    # Invalid examples
    for example in sorted(EXAMPLES_DIR.glob("config-invalid*.json")):
        results.append(validate_example(schema, example, should_pass=False))

    print()
    passed = sum(results)
    total = len(results)
    print(f"Results: {passed}/{total} passed")

    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
