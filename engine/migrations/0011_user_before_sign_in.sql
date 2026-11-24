-- A user made from the Workspace directory has not signed in yet (D-106 point 8).
ALTER TABLE users ALTER COLUMN last_login_at DROP NOT NULL;
