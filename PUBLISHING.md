# Publishing Z# for Maven and Gradle

The Java coordinate is:

```text
com.zombieos:zsharp:1.0.0.0
```

Putting the source repository on GitHub does not publish this coordinate by
itself. The repository includes a GitHub Actions workflow that publishes the
Java library to GitHub Packages whenever a GitHub Release is published. It can
also be started manually from the repository's Actions page.

## Publishing to GitHub Packages

1. Push the complete repository to GitHub.
2. Open the repository's Releases page and publish a release for
   `1.0.0.0`.
3. The `Publish Java package` workflow verifies the Java library and runs the
   Maven deployment using the repository's automatic `GITHUB_TOKEN`.
4. Confirm that `com.zombieos:zsharp:1.0.0.0` appears under the repository's
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
3. Choose and add the project's open-source license.
4. Add the required project URL, source-control, developer, source, Javadoc,
   and signing metadata.
5. Create a signing key and Central Portal publishing token.
6. Publish and release the signed bundle through the Central Portal.

Maven Central releases are immutable. A corrected build must use a new version
instead of replacing `1.0.0.0`.
