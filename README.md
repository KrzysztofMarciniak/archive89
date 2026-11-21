# archive89

A lightweight, single-host website archiver written in C89.

archive89 crawls a website, downloads its resources, rewrites links to local paths, and saves a copy that can be browsed offline.

## Features

- Crawls links on the starting website's host only.
- Supports HTTP and HTTPS.
- Rewrites HTML links and resource references.
- Rewrites CSS url(...) references.
- Downloads images, stylesheets, scripts, and other resources.
- Handles relative, absolute, and protocol-relative URLs.
- Preserves URL fragments.
- Separates URLs with different query strings into different local filenames.
- Maintains a metadata tree and request map.
- Handles HTTP 429 rate limiting with automatic retries.
- Configurable request delays and timeouts.
- Optional maximum file count.
- Optional .tar.gz archive creation.
- Terminal progress indicator.

## Requirements

- C compiler supporting C89.
- POSIX-compatible operating system.
- curl (default HTTP fetcher).
- tar (optional, for compressed archives).
- Standard POSIX utilities such as sh and mktemp.

No additional C libraries are required.

## Building

Compile using:

    cc -std=c89 -pedantic -Wall -O2 -o archive89 archive89.c

## Usage

    archive89 <url> [options]
    archive89 --url <url> [options]

### Examples

Archive a website:

    ./archive89 https://example.com

Specify an output directory:

    ./archive89 https://example.com --output ./my-archive

Create a compressed archive:

    ./archive89 https://example.com --tar.gz

Combine options:

    ./archive89 https://example.com \
        --output ./backup \
        --tar.gz

Show help:

    ./archive89 --help

## Options

    --url <url>       Website to archive
    --output <dir>    Output directory
    -o <dir>          Short form of --output
    --tar.gz          Create a compressed tar archive
    --tar             Alias for --tar.gz
    -h, --help        Show help

## Environment Variables

All environment variables are optional.

### Fetcher configuration

    ARCHIVE89_FETCHER
        Custom HTTP fetch command.

    ARCHIVE89_AGENT
        User-Agent string.
        Default: Firefox 140 on Linux.

    ARCHIVE89_REFERER
        HTTP Referer header.

    ARCHIVE89_COOKIE
        Cookie string or cookie file.

    ARCHIVE89_REQ_TIMEOUT
        Per-request timeout in seconds.
        Default: 60.

### Rate limiting

    ARCHIVE89_RATE_WAIT
        Seconds to wait after HTTP 429.
        Default: 60.

    ARCHIVE89_RATE_RETRIES
        Maximum retries per URL.
        Default: 10.

### Crawl behavior

    ARCHIVE89_WAIT
        Delay between crawl and download phases.
        Default: 10.

    ARCHIVE89_EVERY
        Pause after every N requests.
        Default: 10.
        Set to 0 to disable.

    ARCHIVE89_TIMEOUT
        Duration of periodic pauses.
        Default: 3.

    ARCHIVE89_MAX_FILES
        Maximum number of planned files.
        Default: 0 (unlimited).

### Output configuration

    ARCHIVE89_OUTPUT
        Output directory.

    ARCHIVE89_TAR
        Set to 1 to create a .tar.gz archive.

    ARCHIVE89_METADATA
        Metadata directory.
        Default: <output directory>.metadata.

## Custom HTTP Fetcher

The default fetcher uses curl.

A custom fetcher can be configured:

    ARCHIVE89_FETCHER='wget -qO- -U "$ARCHIVE89_AGENT"' \
        ./archive89 https://example.com

The URL is appended as the final argument unless the command contains $1.

A custom fetcher must:

1. Receive the requested URL as its final argument or through $1.
2. Write the response body to standard output.
3. Return an appropriate exit status.

Exit statuses:

    0   Request succeeded.
    75  Rate limited; wait and retry.
    Other values indicate failure.

Custom fetchers are responsible for their own HTTP status handling and timeout behavior.

## How It Works

archive89 performs two main phases.

### Phase 1: Crawl and Map

1. Add the starting URL to the crawl queue.
2. Download crawlable pages.
3. Detect HTML content.
4. Find supported URL attributes.
5. Resolve relative URLs.
6. Ignore URLs belonging to other hosts.
7. Convert accepted URLs to local paths.
8. Add newly discovered resources to the queue.
9. Rewrite references in downloaded content.

Recognized page extensions:

- html
- htm
- xhtml
- shtml
- php
- asp
- aspx
- jsp
- cgi
- css

URLs without an extension are also considered crawlable.

### Phase 2: Download

1. Wait for ARCHIVE89_WAIT seconds.
2. Download remaining pending resources.
3. Rewrite CSS references.
4. Rewrite HTML references when HTML is detected.
5. Save binary resources without content rewriting.
6. Update the request map.

## Link Rewriting

### HTML

Recognized attributes:

- href
- src
- poster
- data-src

Both single-quoted and double-quoted values are supported.

Example:

    <a href="https://example.com/about">About</a>
    <img src="/images/logo.png">

References are converted to paths relative to the saved HTML file.

### CSS

The archiver recognizes CSS url(...) references.

Example:

    body {
        background-image: url("../images/background.jpg");
    }

Same-host resources are converted to local equivalents.

### URL Handling

Supported URL forms include:

    https://example.com/page
    http://example.com/page
    /page
    ../page
    images/photo.jpg
    //example.com/page

Query strings are retained when generating request URLs.

Different query strings produce different local filenames using a hash suffix.

URL fragments are preserved in rewritten references.

## Output Structure

A typical archive produces:

    example.com/
    example.com.metadata/

Example website directory:

    example.com/
    ├── index.html
    ├── about/
    │   └── index.html
    ├── css/
    │   └── style.css
    ├── js/
    │   └── app.js
    └── images/
        └── logo.png

The metadata directory mirrors the website's file layout.

## Metadata

Each file in the metadata tree represents a planned request.

Metadata contents:

    Empty file       Not downloaded.
    Absolute path    Successfully downloaded.

Failed downloads remain represented by empty metadata files.

### Request Map

The file .archive89.map records the request plan.

Example:

    # archive89 plan: 3 requests
    D https://example.com/ example.com/index.html
    P https://example.com/style.css style.css
    F https://example.com/image.png image.png

State indicators:

    P = Pending
    D = Done
    F = Failed

The map is updated during crawling and downloading.

Note: Metadata and the request map are informational tracking files. The current implementation does not automatically resume an interrupted crawl.

## Rate Limiting

When the fetcher returns status 75, archive89 considers the request rate-limited.

It waits and retries according to the configured settings.

Example:

    ARCHIVE89_RATE_WAIT=120 \
    ARCHIVE89_RATE_RETRIES=5 \
    ./archive89 https://example.com

This waits 120 seconds between retries and allows up to five retries per URL.

These mechanisms do not guarantee that a website will permit automated crawling.

## Compressed Archives

Use:

    ./archive89 https://example.com --tar.gz

This creates:

    example.com/
    example.com.metadata/
    example.com.tar.gz

The compressed archive contains the output directory, not the separate metadata directory.

The system tar utility must be installed.

## Limitations

archive89 is a lightweight archiver, not a full browser or website mirroring engine.

Current limitations:

- Only the starting hostname is crawled.
- No JavaScript execution.
- No dynamic page rendering.
- No browser-based link discovery.
- HTML parsing uses simple attribute scanning.
- Only quoted URL attribute values are recognized.
- CSS rewriting handles url(...) references only.
- HTML base elements are not interpreted.
- JavaScript-generated URLs are not discovered.
- robots.txt rules are not automatically evaluated.
- Authentication requires fetcher configuration, such as cookies.
- No automatic resume from previous crawls.
- URL normalization and filename mapping are simplified.
- Redirects and other HTTP edge cases are not comprehensively handled.
- No guarantee that every website will work perfectly offline.

## Security and Responsible Use

Use archive89 only on websites you own or are authorized to archive.

Respect applicable copyright, access restrictions, terms of service, and server resource limits.

The crawler restricts discovered links to the starting hostname, but redirects may be followed by the HTTP fetcher. Configure your fetcher appropriately when strict network-boundary enforcement is required.

Be careful when using custom fetcher commands: ARCHIVE89_FETCHER is executed through /bin/sh.

Avoid placing sensitive credentials directly in shell command lines where they may be exposed through process listings or logs.

## Troubleshooting

### Cannot create output directory

Check that the destination parent exists and that you have permission to write to it.

### Requests fail

Verify that curl is installed and that the website is accessible.

Try a custom fetcher:

    ARCHIVE89_FETCHER='wget -qO- "$1"' \
    ./archive89 https://example.com

### HTTP 429 errors

Increase the rate-limit wait:

    ARCHIVE89_RATE_WAIT=120 \
    ./archive89 https://example.com

### Missing resources

Some websites generate links dynamically through JavaScript or load resources through mechanisms not recognized by the HTML/CSS scanner.

These resources may not be discovered.

### Offline pages are incomplete

Check for missing downloads, external-host dependencies, JavaScript-generated URLs, and unsupported HTML or CSS constructs.

## License

No license has been specified yet.

Add a LICENSE file before distributing this project publicly.
