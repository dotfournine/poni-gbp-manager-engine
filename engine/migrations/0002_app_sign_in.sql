-- Sign-in for the phone app (D-095).

-- A session is of one of two kinds for good: the dashboard's, which a
-- browser holds as a cookie, or the phone app's, which it sends as a bearer
-- token. Every session there is so far is a browser's. The column has no
-- default: whoever writes a session says which kind it is.
ALTER TABLE sessions ADD COLUMN kind text NOT NULL DEFAULT 'browser' CHECK (kind IN ('browser', 'app'));
ALTER TABLE sessions ALTER COLUMN kind DROP DEFAULT;

-- The CSRF token guards the cookie, so only a browser session has one. An
-- app session has none at all, rather than an empty one that a request
-- without the header would equal.
ALTER TABLE sessions ALTER COLUMN csrf_token DROP NOT NULL;
ALTER TABLE sessions ADD CONSTRAINT sessions_csrf_token_by_kind
    CHECK (CASE kind WHEN 'browser' THEN csrf_token IS NOT NULL AND csrf_token <> '' ELSE csrf_token IS NULL END);

-- The one-time code a sign-in of the app ends with, known by its hash only.
-- It is exchanged for a token within a minute, with the PKCE verifier whose
-- S256 challenge started the sign-in; the exchange deletes the row.
CREATE TABLE app_sign_in_codes (
    code_hash      text        PRIMARY KEY,
    user_id        bigint      NOT NULL REFERENCES users (id),
    code_challenge text        NOT NULL,
    -- For the audit entry of the session the code becomes.
    first_sign_in  boolean     NOT NULL,
    expires_at     timestamptz NOT NULL
);
