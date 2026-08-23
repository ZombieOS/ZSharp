# Publishing Z# for Maven and Gradle

The Java coordinate is:

```text
com.zombieos:zsharp:1.0.0.1
```

Putting the source repository on GitHub does not publish this coordinate by
itself. The repository includes a GitHub Actions workflow that publishes the
Java library to GitHub Packages whenever a GitHub Release is published. It can
also be started manually from the repository's Actions page.

## Publishing to GitHub Packages

1. Push the complete repository to GitHub.
2. Open the repository's Releases page and publish a release for
   `1.0.0.1`.
3. The `Publish Java package` workflow verifies the Java library and runs the
   Maven deployment using the repository's automatic `GITHUB_TOKEN`.
4. Confirm that `com.zombieos:zsharp:1.0.0.1` appears under the repository's
   Packages section.

GitHub Packages requires authentication when users download public as well as
private Maven packages. Consumers need a GitHub personal access token (classic)
with `read:packages` and must add the repository URL to Maven or Gradle.

## Publishing to Maven Central

Maven Central is more convenient for consumers because Gradle projects can use
`mavenCentral()` without GitHub repository credentials. It is a separate
publication process:

1. Create a Central Portal account.
2. Prove ownership of the `com.zombieos` namespace.
3. Install GnuPG and create a signing key for `Jack Johnson
   <jack.johnson@zombieos.com>`.
4. Publish the public key to a public key server and keep the private key and
   passphrase secret.
5. Generate a Central Portal publishing token and add it under the `central`
   server ID in your local Maven `settings.xml`.
6. From `java`, run `mvn -Pcentral deploy`. This signs the JAR, sources,
   Javadocs, and POM, then uploads `central-bundle.zip` for validation. It does
   not automatically release the deployment; review and publish it in the
   Central Portal.

The repository already contains the Apache 2.0 license, project URL,
source-control information, developer details, source and Javadoc attachment,
GPG signing, and Central publishing configuration. Signing secrets must never
be committed to the repository.

The main Java JAR also contains checksum-protected runtimes below
`META-INF/zsharp/runtime/` for Windows x64/ARM64, Linux x64/ARM64, and macOS
Intel/Apple Silicon. Before publishing, verify that every platform directory
contains its `zsharp` executable (or `zsharp.exe` on Windows) and matching
`.sha256` file. Applications may then bundle the dependency with JarJar,
Shadow, or an equivalent tool as long as that resource path is preserved.

### Creating the signing key on Windows

Install Gpg4win from the official [GnuPG download page](https://gnupg.org/download/),
open a new PowerShell window, and run:

```text
gpg --full-generate-key
```

Choose RSA, at least 3072 bits, a two-year expiration, the name `Jack Johnson`,
the email `jack.johnson@zombieos.com`, and a strong passphrase. Then find the
full fingerprint and publish only the public key:

```text
gpg --list-secret-keys --keyid-format long
gpg --keyserver keys.openpgp.org --send-keys FULL_FINGERPRINT
```

Keep the private key, passphrase, and generated revocation certificate in a
secure backup. The full public fingerprint is safe to share and is useful when
configuring or troubleshooting a release.

The Z# 1.0.0.1 release key is:

```text
0A37 6B4C DA2F 3F60 5035  4EDA 624E 0A52 1DD9 6374
```

Its public key is published through `keys.openpgp.org`. The Maven release
profile explicitly selects its primary signing key ID, `624E0A521DD96374`.

### Manual Central Portal upload

A prepared release bundle uses the Maven repository layout and is written to:

```text
out/zsharp-java-1.0.0.1-central-bundle.zip
```

In the Central Portal, choose **Publish Component**, use
`com.zombieos:zsharp:1.0.0.1` as the deployment name, and upload that ZIP.
Uploading starts validation but does not make the release public. After the
deployment reaches **VALIDATED**, review it and press **Publish** separately to
perform the permanent Maven Central release.

Maven Central releases are immutable. A corrected build must use a new version
instead of replacing `1.0.0.1`.
