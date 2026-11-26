-- A report schedule belongs to its location (D-111 point 3): it has no time
-- zone of its own, since its runs and its reports' periods follow the
-- location's (D-109), and it gains its own recipients and, if it says so,
-- its own sections (null: the location's report settings decide). A
-- schedule that is on needs a recipient.
--
-- No rows are converted and nothing refuses a database here: 0018 refused
-- any database with a location, and a schedule needs one, so the table is
-- empty and the new required column needs no default.

ALTER TABLE report_schedules
    DROP COLUMN time_zone,
    ADD COLUMN recipients text[] NOT NULL CHECK (array_position(recipients, NULL) IS NULL AND cardinality(recipients) <= 20),
    ADD COLUMN sections   text[] CHECK (sections IS NULL OR (cardinality(sections) > 0 AND array_position(sections, NULL) IS NULL
                                                             AND sections <@ ARRAY['summary', 'at_a_glance', 'reviews', 'topics',
                                                                                   'employee_recognition', 'highlights', 'visibility'])),
    ADD CONSTRAINT report_schedules_enabled_recipients_check CHECK (NOT enabled OR cardinality(recipients) > 0);
