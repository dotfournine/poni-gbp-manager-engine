-- Push notifications to the phone app (D-097).

-- A device is the APNs token an app session gave: it lives as long as that
-- session, so signing out, a session that ends and a user who is disabled
-- stop its pushes. One per session; a token belongs to one session at a
-- time (a shared phone changes hands). The token is the lower-case hex APNs
-- hands the app. Only an app session registers one, which the code checks.
CREATE TABLE devices (
    id                 bigint      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    session_token_hash text        NOT NULL UNIQUE REFERENCES sessions (token_hash) ON DELETE CASCADE,
    apns_token         text        NOT NULL UNIQUE CHECK (apns_token ~ '^([0-9a-f]{2}){32,100}$'),
    app_version        text        NOT NULL,
    created_at         timestamptz NOT NULL,
    updated_at         timestamptz NOT NULL
);

-- A rule may push, but only about escalations and system problems: drafts
-- waiting and client reports stay email.
ALTER TABLE notification_rules DROP CONSTRAINT notification_rules_channel_check;
ALTER TABLE notification_rules ADD CONSTRAINT notification_rules_channel_check CHECK (channel IN ('email', 'push'));
ALTER TABLE notification_rules ADD CONSTRAINT notification_rules_push_events_check
    CHECK (channel <> 'push' OR event_type IN ('escalation', 'system_problem'));

-- The ids a push opens in the app. No foreign keys: a draft post may be
-- deleted (D-094), and the log keeps what it said.
ALTER TABLE notification_deliveries ADD COLUMN escalation_id bigint;
ALTER TABLE notification_deliveries ADD COLUMN local_post_id bigint;
