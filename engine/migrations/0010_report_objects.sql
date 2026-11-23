-- A report is one page in the bucket (D-104).
--
-- A report's page is an object in the public bucket, r/<32 random bytes>.<ext>,
-- named on the row by its build right before the upload (page_object); its
-- numbers stay in the database. The draft, send-once and reject workflow of
-- D-085 goes: a built report is `ready`, a withdrawn one `discarded` (its page
-- moved to archive_object), one the retention removed `expired`. Share links
-- go: a report has one link, its object's address. A report's sends are
-- deliveries that name it (report_id) and the person who sent it
-- (requested_by, null for a schedule).

ALTER TABLE reports
    ADD COLUMN page_object text,
    ADD COLUMN archive_object text,
    ADD COLUMN auto_send_status text NOT NULL DEFAULT 'off' CHECK (auto_send_status IN ('off', 'pending', 'done'));
-- Read while the old statuses are still there. Of the reports whose schedule
-- sends by itself, one that is still building keeps that wish (`pending`) and
-- one that was sent is `done`. Everything else is `off`, a draft included:
-- it becomes failed below, and a failed report is never sent.
UPDATE reports SET auto_send_status = CASE status WHEN 'building' THEN 'pending' WHEN 'sent' THEN 'done' ELSE 'off' END WHERE auto_send;

ALTER TABLE notification_deliveries
    ADD COLUMN report_id bigint REFERENCES reports (id),
    ADD COLUMN requested_by bigint REFERENCES users (id);
-- Read while reports.sent_by is still there. The join leaves out a delivery
-- whose report 0008 deleted: its report_id stays null.
UPDATE notification_deliveries d SET report_id = p.id, requested_by = p.sent_by
  FROM reports p
 WHERE d.event_type = 'location_report' AND d.event_key = 'report:' || p.id || ':sent';
CREATE INDEX notification_deliveries_report_idx ON notification_deliveries (report_id, created_at DESC);

ALTER TABLE reports DROP CONSTRAINT reports_status_check, DROP CONSTRAINT reports_built_check;
-- A draft's and a sent report's page was in the database and goes with
-- `html`: they have no object, so nothing could show them. data and built_at
-- stay, so that the figures of a development report remain readable.
UPDATE reports SET status = 'failed', error = 'Built before D-104; generate it again.' WHERE status IN ('draft', 'sent');
-- A rejected report is set aside for good: discarded, without an archive,
-- which is a state a discarded report may be in. Each has its numbers (it was
-- a draft) or its error (it had failed), so the check below holds.
UPDATE reports SET status = 'discarded' WHERE status = 'rejected';
ALTER TABLE reports DROP COLUMN auto_send, DROP COLUMN html, DROP COLUMN hold_reason, DROP COLUMN sent_at, DROP COLUMN sent_by;
-- `ready` needs its numbers and its page's name; `discarding` the archive's
-- name (the step that sets it writes both at once); `discarded` and `expired`
-- numbers or the reason there are none. `building` and `failed` need nothing.
ALTER TABLE reports
    ADD CONSTRAINT reports_status_check CHECK (status IN ('building', 'failed', 'ready', 'discarding', 'discarded', 'expired')),
    ADD CONSTRAINT reports_built_check CHECK (
        (status = 'ready') <= (data IS NOT NULL AND page_object IS NOT NULL)
        AND (status = 'discarding') <= (archive_object IS NOT NULL)
        AND (status IN ('discarded', 'expired')) <= (data IS NOT NULL OR error IS NOT NULL));

-- The share links go although some may still work; 0008 refused to migrate
-- over a working one. Here nothing is deployed: no real bucket has been
-- written to, a development page lies in a local folder, and an object no
-- row names is the clean-up script's to delete (D-104 point 8). The audit
-- entries that name a report_share_link stay as they are.
DROP TABLE report_share_links;
