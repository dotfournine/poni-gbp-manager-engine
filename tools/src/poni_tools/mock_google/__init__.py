"""A local mock of the Google APIs the engine uses, with a test portal (D-087). Development only.

`poni-tools mock-google` serves, on 127.0.0.1 only:

- the consent page of connecting an account (approves at once) and the token endpoint;
- Business Profile: accounts, locations, reviews (list, batchGetReviews, one review) and the reply PUT;
- the Performance API, with made-up numbers;
- Gmail send: a message lands in the portal's outbox and goes nowhere;
- the portal, at /, where a person posts, edits and removes reviews and reads the outbox.

The engine sends these calls here when PONI_GOOGLE_MOCK_URL is set. The businesses are the knowledge base's
made-up test clients; every reviewer and review is made up. The mock has no Pub/Sub (the engine polls) and
no delete endpoint of Google's. It is not part of any production image (.dockerignore).
"""
