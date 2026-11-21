# The backup image: pg_dump of the same major version as the server, plus
# curl for the upload to Cloud Storage.
FROM postgres:17
RUN apt-get update && apt-get install -y --no-install-recommends curl ca-certificates \
    && rm -rf /var/lib/apt/lists/*
COPY backup.sh /usr/local/bin/backup.sh
RUN chmod 755 /usr/local/bin/backup.sh
VOLUME /backups
ENTRYPOINT ["/usr/local/bin/backup.sh"]
CMD ["schedule"]
