-- Auto reply lives in a location's reply settings (D-110): its switch, its
-- daily cap and its pause after failing checks are reply settings now, and
-- the per-star switches and the positive and rating-only flags are gone.
-- Only reviews with text auto reply, of at least 4 or 5 stars. The pauses
-- stay on locations and system_settings: they are the engine's own state,
-- not settings. Nothing is carried over, so a database with locations is
-- rebuilt.

DO $$
BEGIN
    IF EXISTS (SELECT 1 FROM locations) THEN
        RAISE EXCEPTION 'Locations exist: rebuild this database (request section 2; settings are not carried over)';
    END IF;
END
$$;

-- Empty (the block above), so the new columns need no default.
ALTER TABLE reply_settings
    DROP COLUMN auto_reply_positive,
    DROP COLUMN auto_reply_rating_only,
    DROP CONSTRAINT reply_settings_auto_reply_min_stars_check,
    ADD CONSTRAINT reply_settings_auto_reply_min_stars_check CHECK (auto_reply_min_stars IN (4, 5)),
    ADD COLUMN auto_reply_enabled              boolean NOT NULL,
    ADD COLUMN auto_reply_daily_cap            integer NOT NULL CHECK (auto_reply_daily_cap BETWEEN 1 AND 1000),
    ADD COLUMN auto_reply_pause_after_failures integer NOT NULL CHECK (auto_reply_pause_after_failures BETWEEN 1 AND 20);

-- Recreated rather than altered: no migration writes a kb_* row, so the
-- table stays empty until the next seed fills it, and the engine does not
-- start before that.
DROP TABLE kb_reply_defaults;
CREATE TABLE kb_reply_defaults (
    id                              boolean PRIMARY KEY DEFAULT true CHECK (id),
    reply_languages                 text[]  NOT NULL CHECK (cardinality(reply_languages) > 0
                                                            AND reply_languages <@ ARRAY['en', 'zh', 'es']),
    fallback_language               text    NOT NULL CHECK (fallback_language IN ('en', 'zh', 'es')),
    signature                       text    NOT NULL CHECK (char_length(signature) <= 200 AND signature !~ '[\r\n]'),
    sign_off_name                   text    NOT NULL CHECK (char_length(sign_off_name) <= 200),
    length                          jsonb   NOT NULL CHECK (jsonb_typeof(length) = 'object'),
    example_replies                 text[]  NOT NULL CHECK (array_position(example_replies, NULL) IS NULL),
    promos_allowed                  boolean NOT NULL,
    contact_allowed                 boolean NOT NULL,
    temperature                     float8  NOT NULL CHECK (temperature BETWEEN 0 AND 2),
    auto_reply_min_stars            integer NOT NULL CHECK (auto_reply_min_stars IN (4, 5)),
    auto_reply_delay_min_minutes    integer NOT NULL CHECK (auto_reply_delay_min_minutes BETWEEN 0 AND 1440),
    auto_reply_delay_max_minutes    integer NOT NULL CHECK (auto_reply_delay_max_minutes BETWEEN 0 AND 1440),
    auto_reply_window_start         text    NOT NULL CHECK (auto_reply_window_start ~ '^(([01][0-9]|2[0-3]):[0-5][0-9])?$'),
    auto_reply_window_end           text    NOT NULL CHECK (auto_reply_window_end ~ '^(([01][0-9]|2[0-3]):[0-5][0-9])?$'),
    auto_reply_enabled              boolean NOT NULL,
    auto_reply_daily_cap            integer NOT NULL CHECK (auto_reply_daily_cap BETWEEN 1 AND 1000),
    auto_reply_pause_after_failures integer NOT NULL CHECK (auto_reply_pause_after_failures BETWEEN 1 AND 20)
);

-- The switch, the cap and the pause after failures moved to reply_settings.
ALTER TABLE locations
    DROP COLUMN auto_reply_enabled,
    DROP COLUMN auto_reply_five_star,
    DROP COLUMN auto_reply_four_star,
    DROP COLUMN auto_reply_daily_cap,
    DROP COLUMN auto_reply_pause_after_failures;

-- A run's width is its minimum of stars alone.
ALTER TABLE go_live_runs
    DROP COLUMN auto_reply_positive,
    DROP COLUMN auto_reply_rating_only;
