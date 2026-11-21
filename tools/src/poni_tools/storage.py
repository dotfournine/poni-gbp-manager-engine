"""Where published knowledge archives are kept (D-078 point 8): a Cloud Storage bucket.

In production the seed step downloads the archive it is told to as the VM's
service account; the owner uploads with their own Google credentials. A
local folder stands in for the bucket in development and in tests: give a
path (or file://path) where a bucket's name is expected.
"""

from __future__ import annotations

from pathlib import Path
from typing import Protocol


class StorageError(Exception):
    pass


class Store(Protocol):
    """A place archives are read from and written to, by name."""

    description: str

    def read(self, name: str) -> bytes: ...

    def write_new(self, name: str, data: bytes) -> None:
        """Stores `data` under `name`; refuses if something is stored there already."""


class FolderStore:
    def __init__(self, folder: Path) -> None:
        self.folder = folder
        self.description = str(folder)

    def read(self, name: str) -> bytes:
        try:
            return (self.folder / name).read_bytes()
        except FileNotFoundError:
            raise StorageError(f"{self.folder} has no archive named {name}") from None

    def write_new(self, name: str, data: bytes) -> None:
        self.folder.mkdir(parents=True, exist_ok=True)
        try:
            with (self.folder / name).open("xb") as file:
                file.write(data)
        except FileExistsError:
            raise StorageError(
                f"{self.folder} already has an archive named {name}; a published archive is never replaced"
            ) from None


class BucketStore:
    """A Cloud Storage bucket, with the credentials of whoever runs this (application default credentials)."""

    def __init__(self, bucket: str) -> None:
        self.bucket = bucket
        self.description = f"gs://{bucket}"

    def _bucket(self):
        # Imported here: checking, migrating and seeding from a folder need no Google library.
        from google.cloud import storage

        return storage.Client().bucket(self.bucket)

    def read(self, name: str) -> bytes:
        from google.api_core import exceptions

        try:
            return self._bucket().blob(name).download_as_bytes()
        except exceptions.NotFound:
            raise StorageError(f"gs://{self.bucket} has no archive named {name}") from None
        except exceptions.GoogleAPIError as error:
            raise StorageError(f"gs://{self.bucket}/{name} could not be read: {error}") from None

    def write_new(self, name: str, data: bytes) -> None:
        from google.api_core import exceptions

        try:
            # if_generation_match=0: only if no object of that name exists.
            self._bucket().blob(name).upload_from_string(data, content_type="application/gzip", if_generation_match=0)
        except exceptions.PreconditionFailed:
            raise StorageError(
                f"gs://{self.bucket} already has an archive named {name}; a published archive is never replaced"
            ) from None
        except exceptions.GoogleAPIError as error:
            raise StorageError(f"gs://{self.bucket}/{name} could not be written: {error}") from None


def store_for(location: str) -> Store:
    """`gs://name` or a bare bucket name is a bucket; a path (absolute, ./relative or file://) is a folder."""
    if location.startswith("file://"):
        return FolderStore(Path(location.removeprefix("file://")))
    if location.startswith(("/", "./", "../", "~")):
        return FolderStore(Path(location).expanduser())
    return BucketStore(location.removeprefix("gs://").rstrip("/"))
