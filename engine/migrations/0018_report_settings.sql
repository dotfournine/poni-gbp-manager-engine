-- A location's report settings (D-111): a third bundle beside its content
-- and reply settings (D-107), one complete row per location, written from
-- the knowledge base's report defaults at discovery. A report records the
-- format and the sections it was created with. The knowledge base's reply
-- defaults can no longer switch auto reply on: discovery copies that row
-- into every new location. Nothing is carried over, so a database with
-- locations is rebuilt.

DO $$
BEGIN
    IF EXISTS (SELECT 1 FROM locations) THEN
        RAISE EXCEPTION 'Locations exist: rebuild this database (request section 2; settings are not carried over)';
    END IF;
END
$$;

CREATE TABLE report_settings (
    location_id bigint PRIMARY KEY REFERENCES locations (id),
    format      text   NOT NULL CHECK (format IN ('html', 'pdf')),
    sections    text[] NOT NULL CHECK (cardinality(sections) > 0 AND array_position(sections, NULL) IS NULL
                                       AND sections <@ ARRAY['summary', 'at_a_glance', 'reviews', 'topics', 'employee_recognition',
                                                             'highlights', 'visibility']),
    created_at  timestamptz NOT NULL,
    updated_at  timestamptz NOT NULL
);

-- No migration writes a kb_* row: the table stays empty until the next seed
-- fills it, and the engine does not start before that.
CREATE TABLE kb_report_defaults (
    id       boolean PRIMARY KEY DEFAULT true CHECK (id),
    format   text    NOT NULL CHECK (format IN ('html', 'pdf')),
    sections text[]  NOT NULL CHECK (cardinality(sections) > 0 AND array_position(sections, NULL) IS NULL
                                     AND sections <@ ARRAY['summary', 'at_a_glance', 'reviews', 'topics', 'employee_recognition',
                                                           'highlights', 'visibility'])
);

-- Empty (a report needs a location, and the block above refused any), so the new columns need no default.
ALTER TABLE reports
    ADD COLUMN format   text   NOT NULL CHECK (format IN ('html', 'pdf')),
    ADD COLUMN sections text[] NOT NULL CHECK (cardinality(sections) > 0 AND array_position(sections, NULL) IS NULL
                                               AND sections <@ ARRAY['summary', 'at_a_glance', 'reviews', 'topics',
                                                                     'employee_recognition', 'highlights', 'visibility']);

ALTER TABLE kb_reply_defaults ADD CONSTRAINT kb_reply_defaults_auto_reply_enabled_check CHECK (NOT auto_reply_enabled);
