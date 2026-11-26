-- A location's settings are two flat bundles, named for what reads them
-- (D-107): content_settings and reply_settings, one complete row each,
-- with one typed column per member. Lists are text[] and keep their order;
-- the reply lengths stay one jsonb document. The knowledge base's defaults
-- take the same shape. Nothing is carried over: settings written in the
-- old shape are not converted, so a database with locations is rebuilt.

DO $$
BEGIN
    IF EXISTS (SELECT 1 FROM locations) THEN
        RAISE EXCEPTION 'Locations exist: rebuild this database (request section 2; settings are not carried over)';
    END IF;
END
$$;

DROP TABLE reply_settings;

CREATE TABLE content_settings (
    location_id     bigint  PRIMARY KEY REFERENCES locations (id),
    business_facts  text    NOT NULL CHECK (char_length(business_facts) <= 2000),
    base_style      text    NOT NULL CHECK (char_length(base_style) <= 2000),
    emoji_allowed   boolean NOT NULL,
    emoji_max       integer NOT NULL CHECK (emoji_max BETWEEN 0 AND 10),
    emoji_preferred text[]  NOT NULL CHECK (array_position(emoji_preferred, NULL) IS NULL),
    words_to_avoid  text[]  NOT NULL CHECK (array_position(words_to_avoid, NULL) IS NULL),
    style_notes     text[]  NOT NULL CHECK (array_position(style_notes, NULL) IS NULL),
    chinese_variant text    CHECK (chinese_variant IN ('zh-CN', 'zh-HK', 'zh-TW')),
    english_variant text    CHECK (english_variant IN ('en-US', 'en-CA')),
    audience        text    NOT NULL CHECK (char_length(audience) <= 500),
    created_at      timestamptz NOT NULL,
    updated_at      timestamptz NOT NULL
);

CREATE TABLE reply_settings (
    location_id                  bigint  PRIMARY KEY REFERENCES locations (id),
    reply_languages              text[]  NOT NULL CHECK (cardinality(reply_languages) > 0
                                                         AND reply_languages <@ ARRAY['en', 'zh', 'es']),
    fallback_language            text    NOT NULL CHECK (fallback_language IN ('en', 'zh', 'es')),
    signature                    text    NOT NULL CHECK (char_length(signature) <= 200 AND signature !~ '[\r\n]'),
    sign_off_name                text    NOT NULL CHECK (char_length(sign_off_name) <= 200),
    length                       jsonb   NOT NULL CHECK (jsonb_typeof(length) = 'object'),
    example_replies              text[]  NOT NULL CHECK (array_position(example_replies, NULL) IS NULL),
    promos_allowed               boolean NOT NULL,
    contact_allowed              boolean NOT NULL,
    temperature                  float8  NOT NULL CHECK (temperature BETWEEN 0 AND 2),
    auto_reply_positive          boolean NOT NULL,
    auto_reply_rating_only       boolean NOT NULL,
    auto_reply_min_stars         integer NOT NULL CHECK (auto_reply_min_stars BETWEEN 1 AND 5),
    auto_reply_delay_min_minutes integer NOT NULL CHECK (auto_reply_delay_min_minutes BETWEEN 0 AND 1440),
    auto_reply_delay_max_minutes integer NOT NULL CHECK (auto_reply_delay_max_minutes BETWEEN 0 AND 1440),
    auto_reply_window_start      text    NOT NULL CHECK (auto_reply_window_start ~ '^(([01][0-9]|2[0-3]):[0-5][0-9])?$'),
    auto_reply_window_end        text    NOT NULL CHECK (auto_reply_window_end ~ '^(([01][0-9]|2[0-3]):[0-5][0-9])?$'),
    created_at                   timestamptz NOT NULL,
    updated_at                   timestamptz NOT NULL
);

-- Who the customers are is a content setting now.
ALTER TABLE locations DROP COLUMN audience;

-- The knowledge base's defaults, in the bundles' shape: one row each.
ALTER TABLE kb_defaults DROP COLUMN voice, DROP COLUMN localization, DROP COLUMN policy;

CREATE TABLE kb_content_defaults (
    id              boolean PRIMARY KEY DEFAULT true CHECK (id),
    business_facts  text    NOT NULL CHECK (char_length(business_facts) <= 2000),
    base_style      text    NOT NULL CHECK (char_length(base_style) <= 2000),
    emoji_allowed   boolean NOT NULL,
    emoji_max       integer NOT NULL CHECK (emoji_max BETWEEN 0 AND 10),
    emoji_preferred text[]  NOT NULL CHECK (array_position(emoji_preferred, NULL) IS NULL),
    words_to_avoid  text[]  NOT NULL CHECK (array_position(words_to_avoid, NULL) IS NULL),
    style_notes     text[]  NOT NULL CHECK (array_position(style_notes, NULL) IS NULL),
    chinese_variant text    CHECK (chinese_variant IN ('zh-CN', 'zh-HK', 'zh-TW')),
    english_variant text    CHECK (english_variant IN ('en-US', 'en-CA')),
    audience        text    NOT NULL CHECK (char_length(audience) <= 500)
);

CREATE TABLE kb_reply_defaults (
    id                           boolean PRIMARY KEY DEFAULT true CHECK (id),
    reply_languages              text[]  NOT NULL CHECK (cardinality(reply_languages) > 0
                                                         AND reply_languages <@ ARRAY['en', 'zh', 'es']),
    fallback_language            text    NOT NULL CHECK (fallback_language IN ('en', 'zh', 'es')),
    signature                    text    NOT NULL CHECK (char_length(signature) <= 200 AND signature !~ '[\r\n]'),
    sign_off_name                text    NOT NULL CHECK (char_length(sign_off_name) <= 200),
    length                       jsonb   NOT NULL CHECK (jsonb_typeof(length) = 'object'),
    example_replies              text[]  NOT NULL CHECK (array_position(example_replies, NULL) IS NULL),
    promos_allowed               boolean NOT NULL,
    contact_allowed              boolean NOT NULL,
    temperature                  float8  NOT NULL CHECK (temperature BETWEEN 0 AND 2),
    auto_reply_positive          boolean NOT NULL,
    auto_reply_rating_only       boolean NOT NULL,
    auto_reply_min_stars         integer NOT NULL CHECK (auto_reply_min_stars BETWEEN 1 AND 5),
    auto_reply_delay_min_minutes integer NOT NULL CHECK (auto_reply_delay_min_minutes BETWEEN 0 AND 1440),
    auto_reply_delay_max_minutes integer NOT NULL CHECK (auto_reply_delay_max_minutes BETWEEN 0 AND 1440),
    auto_reply_window_start      text    NOT NULL CHECK (auto_reply_window_start ~ '^(([01][0-9]|2[0-3]):[0-5][0-9])?$'),
    auto_reply_window_end        text    NOT NULL CHECK (auto_reply_window_end ~ '^(([01][0-9]|2[0-3]):[0-5][0-9])?$')
);

-- A voice template sets some members of each bundle: two sparse documents.
-- Recreated rather than altered, so that a seeded database needs no rows
-- written here; the next seed fills it (a migration seeds no knowledge).
DROP TABLE kb_voice_templates;
CREATE TABLE kb_voice_templates (
    key         text    PRIMARY KEY,
    "position"  integer NOT NULL,
    label       text    NOT NULL,
    description text    NOT NULL,
    content     jsonb   NOT NULL CHECK (jsonb_typeof(content) = 'object'),
    reply       jsonb   NOT NULL CHECK (jsonb_typeof(reply) = 'object')
);
