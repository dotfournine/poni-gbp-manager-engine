-- An audit entry names the location it is about (D-103).
--
-- audit::record fills it in when the entry is written, from the object as it
-- is then, so an entry keeps its location after the object is deleted. The
-- entries written before stay without one: nothing changes an audit entry
-- (the trigger of 0001_baseline.sql), so the check below is NOT VALID and holds
-- only for what is written from now on.

ALTER TABLE audit_log ADD COLUMN location_id bigint REFERENCES locations (id);

CREATE INDEX audit_log_location_idx ON audit_log (location_id, occurred_at DESC, id DESC) WHERE location_id IS NOT NULL;

-- The kinds of object that always belong to one location (AuditEntry.location_id
-- in api/openapi.yaml). A notification rule may be for every location, and
-- `location.discovery_requested` is about no one location (no object_id).
ALTER TABLE audit_log ADD CONSTRAINT audit_log_location_check CHECK (
    location_id IS NOT NULL
    OR (object_type NOT IN ('review', 'draft', 'escalation', 'local_post', 'report', 'report_share_link',
                            'report_schedule', 'go_live_run')
        AND NOT (object_type = 'location' AND object_id <> ''))
) NOT VALID;
