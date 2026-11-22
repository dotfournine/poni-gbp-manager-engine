-- The address of the Google account a stored credential belongs to (D-100),
-- as the verified email claim of the ID token of the connection's own code
-- exchange named it. It is replaced with the token on every connection, and
-- email is sent from the mailbox's. A credential stored before has none: the
-- status page shows it as unknown until the account is connected again.
ALTER TABLE credentials ADD COLUMN account_email text;
