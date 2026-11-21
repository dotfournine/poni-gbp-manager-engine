-- An edit of a post that is at Google (D-099): a change a person proposes to
-- a post Google has, kept beside the post until a person approved it and
-- Google accepted it. The post's own row is untouched meanwhile: its content
-- stays what Google has.
--
-- A row is an open edit, and a post has at most one (the primary key). It
-- moves  draft -> approved -> sending  and from each of the last two back to
-- draft with a reason; when Google accepted it, or a person discarded it,
-- the row is deleted and the audit log keeps what it said. So the table
-- holds no history.
--
-- content and approved_content are domain::LocalPostContent as the engine
-- writes it, as on local_posts; the approval and the claim compare the two.

-- A number for each claim of an edit by the job that sends it, never used
-- twice: the job's bookkeeping names the claim it belongs to, so that a run
-- that comes back late cannot settle an edit another run has since claimed.
CREATE SEQUENCE local_post_edit_claims;

CREATE TABLE local_post_edits (
    -- Only posts that Google has get an edit, and none of those is ever
    -- deleted, with one exception: a row the sync stored a moment before a
    -- send's own answer named the same post (local_post_send_accepted). An
    -- edit written for that row in those seconds goes with it.
    local_post_id    bigint      PRIMARY KEY REFERENCES local_posts (id) ON DELETE CASCADE,
    status           text        NOT NULL CHECK (status IN ('draft', 'approved', 'sending')),
    status_reason    text,
    content          jsonb       NOT NULL,
    checks           jsonb       NOT NULL DEFAULT '[]',
    -- Who wrote the edit, or replaced it last.
    written_by       bigint      NOT NULL REFERENCES users (id),
    written_at       timestamptz NOT NULL,
    decided_by       bigint      REFERENCES users (id),
    decided_at       timestamptz,
    approved_content jsonb,
    send_started_at  timestamptz,
    claim            bigint,
    -- Which upload the edit names, held to a file that was uploaded.
    media_id         bigint      GENERATED ALWAYS AS ((content->>'media_id')::bigint) STORED
                                 REFERENCES local_post_media (id),
    -- Nothing is on its way to Google without a person's approval of its content.
    CONSTRAINT local_post_edits_approval_check
        CHECK ((status IN ('approved', 'sending')) <= (decided_by IS NOT NULL AND decided_at IS NOT NULL AND approved_content IS NOT NULL)),
    CONSTRAINT local_post_edits_sending_check
        CHECK ((status = 'sending') <= (send_started_at IS NOT NULL AND claim IS NOT NULL)),
    -- An edit is content a draft can hold: no alert.
    CONSTRAINT local_post_edits_content_check CHECK (
        coalesce(content->>'topic_type', '') IN ('standard', 'event', 'offer')
        AND jsonb_typeof(content->'summary') IS NOT DISTINCT FROM 'string'
        AND jsonb_typeof(content->'language_code') IS NOT DISTINCT FROM 'string'),
    -- The two times in one spelling, as on local_posts: they are compared as text with the post's.
    CONSTRAINT local_post_edits_content_times_check CHECK (
        content->>'scheduled_time' IS NOT DISTINCT FROM
            to_char((content->>'scheduled_time')::timestamptz AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS.US"Z"')
        AND content#>>'{event,recurrence,until}' IS NOT DISTINCT FROM
            to_char((content#>>'{event,recurrence,until}')::timestamptz AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS.US"Z"'))
);
